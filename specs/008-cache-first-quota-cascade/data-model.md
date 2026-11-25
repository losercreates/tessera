# Data Model & State Transitions: Cache-First Pipeline Ordering & Quota-Aware Cascade Failover

**Feature**: `008-cache-first-quota-cascade`  
**Date**: 2026-11-25  
**Spec Reference**: [spec.md](spec.md)

---

## 1. End-to-End Pipeline Topology

```mermaid
flowchart TD
    Req["Inbound ChatCompletionRequest"] --> Cache{"TwoTierCache Lookup\n(unless req.no_cache)"}
    Cache -- "Tier 1 Exact HIT" --> ReturnHit1["Return Cached Response\n(0 Quota Tokens Deducted)"]
    Cache -- "Tier 2 Semantic HIT" --> ReturnHit2["Return Cached Response\n(0 Quota Tokens Deducted)"]
    Cache -- "Cache MISS / Bypass" --> Resolve["ModelRouter::ResolveCascade(model)"]
    Resolve -- "Unknown / No Cascade" --> ErrInvalid["Status::INVALID_ARGUMENT"]
    Resolve -- "Cascade Found [T0..Tn-1]" --> Loop["CascadeRouter::Complete() Candidate i"]

    Loop --> Breaker{"breaker[i]->CanAttempt()"}
    Breaker -- "OPEN / Busy" --> DirectBypass["stats.direct_bypasses++\nsaw_upstream_or_open = true"] --> NextTier["Advance to i + 1"]
    Breaker -- "CLOSED / Probe Slot" --> Quota{"rate_limiter->CheckAndConsume(prov_i)"}

    Quota -- "Denied (!allowed)" --> ReleaseProbe["breaker[i]->CancelProbe()\nstats.rate_limit_skips++\nTelemetry::RateLimitSkip(prov_i)\nsaw_local_rate_limit = true"] --> FailoverMark["progressed_past_rejection = true"] --> NextTier

    Quota -- "Admitted" --> Execute["client[i]->Execute(upstream_req)"]
    Execute --> Telemetry["Telemetry: RecordLatency + RequestsTotal(prov_i, status)"]
    Telemetry -- "2xx Success" --> BreakerOk["breaker[i]->RecordSuccess()"] --> CacheStore["TwoTierCache::Store(req, resp)"] --> ReturnOk["Status::OK"]
    Telemetry -- "429 / 5xx / Timeout" --> BreakerFail["breaker[i]->RecordFailure(status)\nsaw_upstream_or_open = true"] --> FailoverMark

    NextTier -- "More Candidates" --> Loop
    NextTier -- "All Exhausted" --> FinalErr{"Only Local Quota Exhausted?"}
    FinalErr -- "Yes" --> ErrRL["Status::RESOURCE_EXHAUSTED\n(min retry_after_ms)"]
    FinalErr -- "No" --> ErrUnavail["Status::UNAVAILABLE\n(last upstream error)"]
```

---

## 2. Modified & Extended Entities

### 2.1 `CascadeTarget` ([`include/tessera/routing/circuit_types.hpp`](file:///home/mayukh/projects/tessera/include/tessera/routing/circuit_types.hpp))

Represents an individual candidate stage inside a cascade chain.

| Field | Type | Default | Semantics |
|---|---|---|---|
| `client` | `std::shared_ptr<IUpstreamClient>` | `nullptr` | Upstream OpenAI-compatible client adapter |
| `breaker` | `std::shared_ptr<ICircuitBreaker>` | `nullptr` | Provider-isolated 3-state circuit breaker |
| `target_model` | `std::string` | `""` | Optional model identifier override forwarded to provider |
| `priority` | `uint32_t` | `0` | Evaluation priority order (0 = highest priority) |
| `rate_limiter` | `std::shared_ptr<DistributedRateLimiter>` | `nullptr` | Optional per-target rate limiter overriding cascade-level limiter |

### 2.2 `CascadeRouterStats` ([`include/tessera/routing/cascade_router.hpp`](file:///home/mayukh/projects/tessera/include/tessera/routing/cascade_router.hpp))

Counters snapshot exposing granular cascade behavior across failover, circuit skips, and local quota skips.

| Field | Type | Semantics |
|---|---|---|
| `total_requests` | `uint64_t` | Total invocations entering `CascadeRouter::Complete()` |
| `successful_requests` | `uint64_t` | Requests completed with HTTP 2xx on any candidate tier |
| `failed_requests` | `uint64_t` | Requests failing after exhausting all tiers |
| `failover_attempts` | `uint64_t` | Candidate transitions triggered after an upstream error OR a local rate-limit skip |
| `direct_bypasses` | `uint64_t` | Candidate skips caused by `breaker->CanAttempt() == false` |
| `rate_limit_skips` | `uint64_t` | **New**: Candidate skips caused by `rate_limiter->CheckAndConsume() == false` |
| `all_providers_failed` | `uint64_t` | Requests where every target in the route failed or was skipped |

### 2.3 `FailoverRouterStats` ([`include/tessera/routing/circuit_types.hpp`](file:///home/mayukh/projects/tessera/include/tessera/routing/circuit_types.hpp))

Preserves legacy 2-tier metrics while exposing local quota skip attribution:

| Field | Type | Semantics |
|---|---|---|
| `rate_limit_skips` | `uint64_t` (default `0`) | **New**: Cumulative local quota-triggered skips reported via underlying cascade |

### 2.4 `TelemetryRegistry` Counters ([`include/tessera/telemetry/telemetry_registry.hpp`](file:///home/mayukh/projects/tessera/include/tessera/telemetry/telemetry_registry.hpp))

Adds local rate-limit rejection attribution per provider cleanly separated from upstream network requests and latency histograms:

- `gateway_ratelimit_skips_total{provider="<name>"}` (`COUNTER`): Incremented via `IncrementRateLimitSkipsTotal(provider)` when local distributed quota prevents dispatching to `<name>`.

---

## 3. Tier Admission State Machine

```mermaid
stateDiagram-v2
    [*] --> CheckBreaker: Evaluate Target i
    CheckBreaker --> CircuitBypassed: "CanAttempt() == false"
    CheckBreaker --> CheckQuota: "CanAttempt() == true"
    CheckQuota --> QuotaExhausted: "!CheckAndConsume().allowed\n[CancelProbe() called]"
    CheckQuota --> Dispatched: "CheckAndConsume().allowed"
    Dispatched --> Succeeded: "Upstream 2xx\n[RecordSuccess()]"
    Dispatched --> UpstreamFailed: "Upstream 429/5xx/Timeout\n[RecordFailure()]"

    CircuitBypassed --> NextCandidate: "direct_bypasses++"
    QuotaExhausted --> NextCandidate: "rate_limit_skips++\nfailover armed\nbreaker untouched"
    UpstreamFailed --> NextCandidate: "failover armed"
    NextCandidate --> CheckBreaker: "i < N - 1"
```
