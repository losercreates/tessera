# Interface Contract: Cache-First Pipeline & Per-Tier Quota Cascade

**Feature**: `008-cache-first-quota-cascade`  
**Contract Version**: 1.0.0  
**Scope**: Internal Backend & Router Interfaces (`CachedCompletionBackend`, `ModelRouter`, `CascadeRouter`, `FailoverRouter`, `TelemetryRegistry`)

---

## 1. Pipeline Execution Ordering Contract

Gateway startup (`src/main.cpp`) and end-to-end multi-provider deployments MUST compose completion backends in cache-first order:

```text
Request -> CachedCompletionBackend (Tier 1 Exact -> Tier 2 Semantic)
              | (on MISS or req.no_cache)
              v
           ModelRouter (Exact / Wildcard / Default Cascade)
              |
              v
           CascadeRouter (Per-Tier Circuit Breaker -> Per-Tier Rate Limiter -> Upstream HTTP)
              | (on 2xx OK)
              v
           TwoTierCacheService::Store(request, response)
```

### Invariants
1. **Cache Hit Quota Invariant (FR-001, FR-002)**:
   - On any Tier 1 exact-match hit or Tier 2 semantic-match hit (`request.no_cache == false`), `DistributedRateLimiter::CheckAndConsume()` MUST be invoked **0 times**.
   - Even when all configured provider token buckets are at `0` remaining tokens, cached prompts MUST return `Status::OK()` with `response.cache_hit == true`.
2. **Cache-Bypass Invariant**:
   - When `request.no_cache == true`, `CachedCompletionBackend` bypasses cache reads, invokes `ModelRouter` directly, enforces per-tier quotas inside `CascadeRouter`, and does not populate bypassed reads from stale state.
3. **Cache Population on Fallback (FR-009)**:
   - When Tier `k > 0` (secondary/tertiary fallback) returns `Status::OK()`, `CachedCompletionBackend` indexes the response into Tier 1 Exact Cache and Tier 2 Semantic Cache so subsequent matching calls preempt upstream quota consumption.

---

## 2. `CascadeRouter` & `ICircuitBreaker` Admission Contract

### Updated Signatures

```cpp
// include/tessera/routing/circuit_breaker.hpp
class ICircuitBreaker {
 public:
  ...
  // Releases a HALF-OPEN trial probe reservation when local admission denies dispatch
  // prior to network execution. No-op in CLOSED or OPEN states.
  virtual void CancelProbe() noexcept {}
};

// include/tessera/routing/cascade_router.hpp
class CascadeRouter : public ICompletionBackend {
 public:
  CascadeRouter(
      std::vector<CascadeTarget> targets,
      std::string route_id = "default",
      std::shared_ptr<DistributedRateLimiter> rate_limiter = nullptr);

  void SetRateLimiter(std::shared_ptr<DistributedRateLimiter> rate_limiter);
  ...
};

// include/tessera/routing/model_router.hpp
class ModelRouter : public ICompletionBackend {
 public:
  void SetRateLimiter(std::shared_ptr<DistributedRateLimiter> rate_limiter);
  static std::shared_ptr<ModelRouter> CreateFromRegistry(
      const ProviderRegistry& registry,
      std::shared_ptr<DistributedRateLimiter> rate_limiter = nullptr);
};
```

### Per-Candidate Evaluation Algorithm
For `i = 0` to `targets_.size() - 1`:
1. If `targets_[i].client == nullptr`, treat as unavailable and continue.
2. **Circuit Admission**: If `targets_[i].breaker != nullptr && !targets_[i].breaker->CanAttempt()`:
   - Increment `stats_.direct_bypasses`.
   - Record `saw_upstream_failure_or_open = true`.
   - **MUST NOT** call `CheckAndConsume()` (FR-005).
   - Continue to `i + 1`.
3. **Local Quota Admission**:
   - Select `limiter = targets_[i].rate_limiter ? targets_[i].rate_limiter : rate_limiter_`.
   - If `limiter != nullptr`: check `rl = limiter->CheckAndConsume(prov_name, 1.0)`.
   - If `!rl.allowed`:
     - Call `targets_[i].breaker->CancelProbe()` if breaker is non-null.
     - **MUST NOT** call `targets_[i].breaker->RecordFailure(...)` (FR-007).
     - **MUST NOT** call `targets_[i].client->Execute(...)` (FR-006).
     - Increment `stats_.rate_limit_skips` and `TelemetryRegistry::Instance().IncrementRateLimitSkipsTotal(prov_name)`.
     - Set `saw_local_rate_limit = true`, `min_retry_after_ms = std::min(min_retry_after_ms, rl.retry_after_ms)`.
     - Mark `progressed_past_rejection = true` so transitioning to candidate `i + 1` counts as a `failover_attempt`.
     - Continue to `i + 1`.
4. **Failover Attribution**:
   - If `progressed_past_rejection` is true: increment `stats_.failover_attempts`.
5. **Upstream Dispatch**:
   - Invoke `targets_[i].client->Execute(req)`.
   - Attribute latency (`RecordUpstreamLatency(prov_name, resp.latency_ms)`) and request count (`IncrementRequestsTotal(req.model, prov_name, status)`).
   - If `resp.IsSuccess()`: call `breaker->RecordSuccess()`, update circuit gauge, populate response, and return `Status::OK()`.
   - Otherwise: call `breaker->RecordFailure(resp.status_code, last_error)`, update circuit gauge, mark `saw_upstream_failure_or_open = true`, mark `progressed_past_rejection = true`, and continue to `i + 1`.

### Exhaustion Error Contract (FR-010)
After all candidates fail:
- Increment `stats_.failed_requests` and `stats_.all_providers_failed`.
- If `saw_local_rate_limit && !saw_upstream_failure_or_open`:
  - Return `Status::ResourceExhausted("Rate limit exceeded across all providers in route '" + route_id_ + "'. Retry after " + std::to_string(retry_ms) + "ms")`.
- Else:
  - Return `Status::Unavailable("All upstream providers in cascade exhausted: " + last_error)`.

---

## 3. Telemetry Exposition Contract (`TelemetryRegistry`)

Exposes local rate-limit skip counter cleanly without distorting upstream network latency histograms:

```prometheus
# HELP gateway_ratelimit_skips_total Total number of upstream dispatches skipped due to local rate limit exhaustion
# TYPE gateway_ratelimit_skips_total counter
gateway_ratelimit_skips_total{provider="<name>"} <count>
```

- `gateway_requests_total` and `upstream_latency_ms` MUST ONLY record entries when an upstream `client->Execute()` was actually performed (never populated with artificial `0ms` latency on local skips or cache hits).
- `RegisterProvider(name)` initializes `gateway_ratelimit_skips_total{provider="<name>"}` to `0` (mirroring `circuit_breaker_state`), and `Reset()` clears dynamically registered skip counters.
