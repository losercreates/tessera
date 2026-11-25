# Feature Specification: Cache-First Pipeline Ordering & Quota-Aware Cascade Failover

**Feature Branch**: `008-cache-first-quota-cascade`  
**Created**: 2026-11-25  
**Status**: Draft  
**Input**: User description: "Cache-First Pipeline Ordering & Quota-Aware Cascade Failover: Restructure the request execution pipeline so Tier 1 exact-match and Tier 2 semantic-match cache lookups preempt upstream rate-limit deduction, guaranteeing cached hits consume zero provider quota. Move per-provider distributed rate-limit enforcement into each cascade candidate step inside CascadeRouter so local token-bucket exhaustion transparently triggers failover to the next priority provider alongside upstream HTTP 429 and 5xx errors, rather than failing fast on the primary tier. Preserve cache preemption guarantees, zero dropped requests across multi-tier fallback, accurate failure attribution, and full ASan/UBSan cleanliness."

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Zero-Quota Cache Preemption (Priority: P1) 🎯 MVP

As a cost-conscious platform operator, I want every incoming completion request to check both the exact-match and semantic-match response caches **before** evaluating or deducting any upstream provider's rate-limit quota, so that repeated or semantically equivalent prompts never waste external API quota and continue succeeding even when upstream provider quotas are temporarily exhausted.

**Why this priority**: Core economic and availability guarantee. Deducting upstream quota on requests already answered by the cache negates one of the gateway's main cost and throughput protections and unnecessarily rejects cacheable traffic during rate-limit spikes.

**Independent Test**:
1. Configure a provider with a strict per-minute quota of 2 requests.
2. Send an initial unique prompt `P1` to populate the cache (consumes 1 quota unit; 1 unit remaining).
3. Consume the remaining quota unit with a second unique prompt `P2` (0 units remaining).
4. Replay prompt `P1` (exact match) and send a close paraphrase `P1'` (semantic match).
5. Verify both cached queries succeed immediately, while a new uncached prompt `P3` sees the exhausted primary quota.

**Acceptance Scenarios**:
1. **Given** a provider with available quota and an already-cached exact-match completion,  
   **When** a client submits a matching request (without cache bypass),  
   **Then** the gateway returns the cached response immediately and deducts zero quota tokens from any provider.
2. **Given** a provider whose local rate-limit quota is completely exhausted and a semantically equivalent completion stored in the semantic cache,  
   **When** a client submits a matching paraphrased query (without cache bypass),  
   **Then** the gateway serves the completion from the semantic cache rather than rejecting the request as rate-limited.
3. **Given** a client request explicitly flagged to bypass cache reads,  
   **When** the request arrives at the gateway,  
   **Then** cache lookup is skipped and the request proceeds directly to quota-checked upstream routing.

---

### User Story 2 - Quota-Triggered Multi-Tier Cascade Failover (Priority: P1)

As a service owner relying on multi-provider redundancy, I want local per-provider rate-limit enforcement to execute individually for each candidate tier inside the failover chain rather than failing fast at the gateway edge, so that local quota exhaustion on a higher-priority provider transparently triggers failover to the next healthy provider with zero dropped requests.

**Why this priority**: Eliminates artificial single-point-of-failure rejections. Even with fallback providers configured and healthy, checking quotas solely at the outer pipeline edge rejects requests the moment the primary tier fills its bucket instead of using configured fallback capacity.

**Independent Test**:
1. Configure a 3-tier priority cascade (`Provider A -> Provider B -> Provider C`) where Provider A has a 2-request quota, Provider B has a 2-request quota, and Provider C has abundant quota.
2. Dispatch 5 distinct uncached requests concurrently or sequentially.
3. Verify the first 2 complete on Provider A, the next 2 transparently fail over to and complete on Provider B, and the 5th transparently fails over to and completes on Provider C—achieving 5/5 (100%) completions with zero client failures.

**Acceptance Scenarios**:
1. **Given** a priority cascade containing Provider A (local quota exhausted) and Provider B (quota available, healthy),  
   **When** a cache-miss request arrives targeting that cascade,  
   **Then** the gateway skips Provider A without sending an upstream network call and completes the request on Provider B, deducting 1 quota unit only from Provider B.
2. **Given** a 3-tier cascade where Provider A's local quota is exhausted and Provider B fails with a server error or upstream throttling response,  
   **When** a cache-miss request traverses the cascade,  
   **Then** the gateway transitions cleanly through both conditions and completes the request on Provider C with zero dropped requests.
3. **Given** a client request targeting an unmapped or model-specific route whose single provider's local quota is exhausted and has no fallback tier,  
   **When** the request misses cache,  
   **Then** the gateway rejects the request cleanly with an actionable rate-limit error including retry-after timing.

---

### User Story 3 - Accurate Failure Attribution & Telemetry Integrity (Priority: P2)

As an SRE/observability engineer, I want local quota exhaustion skips, upstream provider errors, failover transitions, circuit breaker states, and cache hits to be accurately attributed to their actual causes and providers, so that dashboards and alerts distinguish proactive local throttling from genuine upstream service outages.

**Why this priority**: Prevents false-positive outage alerts and distorted capacity planning. If local rate-limit skips trip outage circuit breakers or attribute metrics to the wrong provider, operators cannot diagnose whether external vendors are healthy or simply at quota.

**Independent Test**:
1. Exhaust Provider A's local rate-limit quota on a 2-tier cascade (`Provider A -> Provider B`).
2. Dispatch additional uncached requests that fail over to Provider B.
3. Inspect provider telemetry and circuit states and confirm: Provider A's circuit stays healthy (CLOSED) and receives zero wasted network requests, failover metrics increment, and upstream latency/completion counts attribute to Provider B.

**Acceptance Scenarios**:
1. **Given** Provider A is bypassed due to local rate-limit quota exhaustion,  
   **When** the request completes on Provider B,  
   **Then** completion latency and upstream success counters are recorded under Provider B, failover attempt counters increment, and Provider A's failure circuit breaker is not tripped as a service crash.
2. **Given** all candidates in a cascade are exhausted across a mix of local quota exhaustion, open circuits, and upstream failures,  
   **When** no tier can serve the request,  
   **Then** the gateway returns a clear service-unavailable or capacity-exhausted error reflecting the final failure state and retry guidance without resource leaks or hangs.
3. **Given** a request succeeds via a secondary or tertiary fallback tier after primary local quota exhaustion or upstream error,  
   **When** the completion is returned,  
   **Then** the response is eligible for caching across both cache tiers so subsequent matching queries incur zero upstream calls and zero quota deductions.

---

### Edge Cases

- **Concurrent Burst at Quota Boundary**: When many concurrent cache-miss requests hit the exact quota boundary of Provider A simultaneously, at most Provider A's exact configured quota is consumed; all excess concurrent requests smoothly divert to Provider B without exceeding Provider A's cap or failing.
- **Exhausted Primary Quota + Open Secondary Circuit**: When Provider A's local quota is exhausted and Provider B's outage circuit is already open, the cascade skips both without network I/O and dispatches straight to Provider C.
- **Zero-Quota Cascade (All Tiers Locally Rate-Limited)**: When every tier in a cascade is locally rate-limited (no upstream network errors occurred), the gateway fails closed cleanly, reports rate-limit exhaustion with the shortest applicable retry-after window, and does not trip any provider's outage circuit breaker.
- **Mixed Cache-Bypass & Normal Traffic**: When a client sends `no_cache` on a rate-limited cascade, it follows the quota-aware failover chain without reading cache, while concurrent normal clients with identical prompts still hit previously populated cache entries at zero quota cost.
- **Rate Limiter Store Degraded/Unavailable**: If the distributed rate-limit store becomes temporarily unreachable, rate checking follows fail-open semantics so failover cascade execution remains resilient rather than blocking healthy upstream traffic.
- **Cache Store Unavailable on Miss/Hit**: If the cache store is temporarily unavailable, the request falls through cleanly to the quota-aware cascade without stalling or corrupting quota counters.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST evaluate exact-match and semantic-match caches prior to performing any upstream provider rate-limit quota check or token deduction (unless the request explicitly specifies cache bypass).
- **FR-002**: System MUST serve cached hits with zero rate-limit quota consumption across all configured providers, regardless of whether any provider's current quota is exhausted.
- **FR-003**: System MUST enforce distributed per-provider rate-limit quotas individually at each candidate evaluation step inside the ordered cascade rather than as a single pre-routing gate targeting only the primary candidate.
- **FR-004**: System MUST treat local provider quota exhaustion on a cascade candidate as a non-fatal, recoverable candidate rejection and automatically continue evaluating subsequent candidates in the priority chain until a tier admits and completes the request.
- **FR-005**: System MUST only deduct rate-limit quota for the specific candidate provider stage(s) evaluated for dispatch, and MUST NOT deduct quota for candidates bypassed due to an open circuit breaker or earlier tier success.
- **FR-006**: System MUST NOT initiate an upstream network call to a provider whose local rate-limit quota check fails.
- **FR-007**: System MUST preserve independent circuit protection semantics such that proactive local quota exhaustion skips do not trigger outage circuit-breaker trips on healthy upstream endpoints, whereas upstream throttling rejections, server errors, and timeouts continue governing circuit transitions.
- **FR-008**: System MUST attribute upstream execution latency, completion status counts, and failover telemetry specifically to the executing or rejecting provider tier, and emit cache-hit telemetry without upstream provider consumption on cached responses.
- **FR-009**: System MUST index successful completions obtained from any failover tier (primary or fallback) into active cache layers (when caching criteria apply) so future matching requests avoid repeating failover or quota expenditure.
- **FR-010**: System MUST return an actionable error response (conveying rate-limit retry guidance when all failures stem from local quotas, or service unavailability when all upstream candidates fail or have open circuits) if every candidate in a cascade is exhausted.
- **FR-011**: System MUST maintain exact quota isolation and thread safety under high concurrent load without memory leaks, undefined behavior, or stale state across threads.
- **FR-012**: System MUST maintain backward-compatible behavior for legacy routing adapters by inheriting cache-first ordering and per-tier quota-aware failover semantics uniformly.

### Key Entities

- **Request Execution Pipeline**: Ordered sequence processing an inbound completion request: (1) Response Cache Lookup & Preemption, (2) Dynamic Route Resolution, (3) Per-Tier Admission Control (Circuit State + Distributed Quota), (4) Upstream Execution & Failover, and (5) Asynchronous/Post-Execution Cache Population.
- **Cascade Candidate Stage**: An individual upstream target in an ordered failover list containing a target platform reference, model mapping, dedicated outage circuit state, and provider quota identity.
- **Tier Admission Decision**: Outcome of pre-dispatch checks for a candidate stage—`ADMITTED` (circuit allows attempt and quota consumed), `CIRCUIT_BYPASSED` (circuit open), or `QUOTA_EXHAUSTED` (local quota depleted, with retry-after interval).

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: **Zero Quota Leakage on Cache Hits**: 100% of requests resolved from exact-match or semantic-match caches deduct 0 rate-limit tokens from any provider quota, even under 100% primary quota saturation.
- **SC-002**: **Zero Dropped Requests Across Quota Failover**: 100% of uncached requests over a multi-tier cascade succeed whenever at least one fallback tier has available quota and healthy status (0 spurious rejections from primary-tier quota exhaustion).
- **SC-003**: **Exact Distributed Quota Compliance**: Under concurrent burst traffic, no individual provider exceeds its configured distributed quota ceiling by even 1 request, while total system throughput reaches the combined quota sum of all active cascade tiers.
- **SC-004**: **Zero False Circuit Trips from Local Throttling**: 0% of proactive local quota exhaustion events falsely transition a provider's outage circuit breaker to open.
- **SC-005**: **Sub-Millisecond Cache & Routing Overhead**: Cache hits continue resolving within established sub-2ms exact-match and sub-20ms semantic-match latency bounds without adding rate-limit store round-trips on cache hits.
- **SC-006**: **Memory & Concurrency Cleanliness**: All automated unit, integration, chaos-failover, and high-concurrency stress suites pass with 100% pass rate and 0 errors or leaks under memory and undefined-behavior sanitizers.

---

## Assumptions

- Distributed per-provider rate-limit enforcement continues using fail-open semantics when the underlying quota store is temporarily unreachable so transient infrastructure hiccups do not halt upstream routing.
- Proactive local token-bucket checks consume 1 request token per candidate dispatch attempt (matching existing per-request quota granularity).
- When all candidates in a cascade fail solely due to local quota exhaustion across single- or multi-tier routes, returning a rate-limit exhaustion error with the shortest applicable retry window gives callers actionable backoff timing without masking upstream outages.
- When a route mixes local quota exhaustion on some tiers with upstream errors or open circuits on remaining tiers, returning service unavailability (while logging full per-tier causes) preserves fault transparency.
- Memory safety, thread safety, coverage thresholds, and sanitizer cleanliness remain governed by the project's existing constitutional verification gates.
