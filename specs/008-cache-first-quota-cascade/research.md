# Research & Architectural Decisions: Cache-First Pipeline Ordering & Quota-Aware Cascade Failover

**Feature**: `008-cache-first-quota-cascade`  
**Date**: 2026-11-25  
**Status**: Resolved (Zero Open Unknowns)

---

## Decision 1: Cache Preemption Placement Relative to Quota Enforcement

- **Decision**: Place `CachedCompletionBackend` (`TwoTierCacheService`) at the outermost request-execution boundary wrapping `ModelRouter`, and move distributed quota enforcement (`DistributedRateLimiter::CheckAndConsume`) inside `CascadeRouter::Complete()` after `ModelRouter` resolves the candidate chain.
- **Rationale**:
  - In the previous wiring (`src/main.cpp`), `RateLimitedBackend` wrapped `CachedCompletionBackend`, causing exact-match and semantic-match cache hits to burn Redis token-bucket quota on every call and fail with `429` even when cached completions were available.
  - Evaluating caches first ensures 100% of Tier 1 and Tier 2 cache hits consume zero upstream tokens, skip Redis Lua rate-limit RTTs completely, and continue succeeding during upstream quota saturation.
  - Leaving `RateLimitedBackend` intact as a library adapter maintains full source/ABI backward compatibility for standalone unit tests (`test_rate_limiter.cpp`, `test_redis_resilience.cpp`).
- **Alternatives Considered**:
  - *Check cache inside `RateLimitedBackend`*: Breaks clean separation of concerns between rate-limiting middleware and caching middleware (`Constitution Principle I: Modular Decoupling`) and still fails to enable multi-tier failover upon primary quota exhaustion. Rejected.

---

## Decision 2: Per-Candidate Rate-Limit Admission Inside `CascadeRouter`

- **Decision**: Attach `std::shared_ptr<DistributedRateLimiter>` optionally to `CascadeRouter` (and per `CascadeTarget`) so every tier `i` in `targets_` evaluates admission sequentially:
  1. `breaker->CanAttempt()` (bypass if `OPEN` or probe busy, without deducting rate-limit quota)
  2. `limiter->CheckAndConsume(provider_name, 1.0)` (if `!allowed`, release any `HALF_OPEN` probe via `breaker->CancelProbe()`, record local rate-limit skip, do **not** call `breaker->RecordFailure()`, do **not** call `client->Execute()`, and advance to tier `i + 1`)
  3. `client->Execute(req)` (record upstream latency + status telemetry + breaker result)
- **Rationale**:
  - Local quota depletion on Tier 1 transparently triggers failover to Tier 2 (`CascadeRouter` loops cleanly through `i = 0..N-1`) with zero dropped calls whenever downstream capacity exists.
  - Checking `breaker->CanAttempt()` before `CheckAndConsume()` guarantees OPEN circuits never drain token buckets for providers we will not call (FR-005).
  - Adding non-blocking `CancelProbe()` when `CheckAndConsume()` denies a `HALF_OPEN` candidate guarantees a locally rate-limited trial thread cannot leave `probe_in_flight_` permanently latched `true` across subsequent requests once tokens refill.
- **Alternatives Considered**:
  - *Pre-select first non-exhausted provider inside `RateLimitedBackend`*: Would duplicate `CascadeRouter` traversal logic, miss retry on mid-flight upstream 429/5xx after Tier 1's local bucket passed, and scatter failover stats. Rejected.
  - *Check `DistributedRateLimiter` before `CircuitBreaker::CanAttempt()`*: Would drain tokens from the Redis bucket for a provider whose circuit breaker is `OPEN`, burning refilling quota capacity unnecessarily. Rejected.

---

## Decision 3: Circuit Breaker Independence & Local Throttling Attribution

- **Decision**: Keep `ICircuitBreaker` strictly scoped to *upstream* failures (`HTTP 429`, `5xx`, socket timeouts). When `DistributedRateLimiter::CheckAndConsume()` denies a tier locally, `CascadeRouter` increments dedicated `rate_limit_skips` metrics (`CascadeRouterStats::rate_limit_skips` and `TelemetryRegistry::IncrementRateLimitSkipsTotal(provider)`) plus `failover_attempts` when moving to subsequent tiers, without modifying the provider's circuit state or emitting synthetic `0ms` latency samples.
- **Rationale**:
  - Local token buckets already replenish smoothly at their configured rate (`refill_rate_per_sec`); tripping a provider's outage circuit breaker (`1000ms–60000ms` exponential backoff) on local throttling would unnecessarily lock out a healthy upstream well past `retry_after_ms` and trigger false outage alerts (`SC-004`).
  - Keeping `upstream_latency_ms` and `gateway_requests_total` tied to actual upstream dispatches (while surfacing `gateway_ratelimit_skips_total` for proactive local skips) gives SREs unambiguous attribution (`FR-008`).
- **Alternatives Considered**:
  - *Record `RecordFailure(429)` on local quota exhaustion*: Violates FR-007 and causes cascade thrashing between token-bucket recovery and circuit-breaker backoff timers. Rejected.

---

## Decision 4: Exhausted-Cascade Error Classification (`RESOURCE_EXHAUSTED` vs `UNAVAILABLE`)

- **Decision**: Inside `CascadeRouter::Complete()`, track per-call outcome flags:
  - `saw_local_rate_limit` (at least one tier rejected by `DistributedRateLimiter`) and `min_retry_after_ms`
  - `saw_upstream_failure_or_open_circuit` (at least one tier failed `Execute()` or was bypassed via `OPEN` circuit)
  When no tier succeeds:
  - If `saw_local_rate_limit && !saw_upstream_failure_or_open_circuit`: return `Status::ResourceExhausted("Rate limit exceeded across all providers in route '<route_id>'. Retry after <min_retry_after_ms>ms")`.
  - Otherwise: return `Status::Unavailable("All upstream providers in cascade exhausted: " + last_error)`.
  - Update `FailoverRouter::Complete()` to propagate `StatusCode::RESOURCE_EXHAUSTED` directly instead of overwriting all errors with `Status::Unavailable`.
- **Rationale**:
  - Single-provider exact routes (`ollama/llama3.2`) and multi-tier cascades whose quotas are simultaneously filled return actionable `RESOURCE_EXHAUSTED` with the earliest recovery retry hint (`FR-010`, User Story 2 Scenario 3).
  - Routes experiencing upstream crashes/outages preserve `UNAVAILABLE`, maintaining 100% compatibility with existing chaos and exhaustion tests (`DualProviderExhaustionReturnsUnavailable`, `AllProvidersExhaustedReturnsUnavailable`).
- **Alternatives Considered**:
  - *Always return `Status::Unavailable`*: Masks actionable retry-after timing for single-provider or rate-saturated routes. Rejected.

---

## Decision 5: `FailoverRouter` Backward Compatibility & `failover_attempts` Accounting

- **Decision**: Fix `CascadeRouter::Complete()` failover attempt counting so progressing past a `direct_bypass` (`OPEN` circuit) does **not** redundantly increment `failover_attempts`, whereas progressing past an upstream execution failure OR a `rate_limit_skip` **does** increment `failover_attempts`. Extend `FailoverRouter` constructor with an optional `std::shared_ptr<DistributedRateLimiter> rate_limiter = nullptr` and propagate `rate_limit_skips` cleanly in `FailoverRouter::Complete()` and `FailoverRouterStats`.
- **Rationale**:
  - Ensures 2-tier legacy callers and tests (`evaluation_harness`, `test_failover_integration`, `test_chaos_mock`) keep exact backward compatibility while also gaining quota-aware failover whenever initialized with a `DistributedRateLimiter` (`FR-012`).
- **Alternatives Considered**:
  - *Deprecate `FailoverRouter` immediately*: Would break existing evaluation harness and chaos tests unnecessarily. Rejected.
