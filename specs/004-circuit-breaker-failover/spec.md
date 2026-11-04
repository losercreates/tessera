# Feature Specification: Circuit Breaker & Multi-Provider Failover

**Feature Branch**: `004-circuit-breaker-failover`  
**Created**: 2026-11-04  
**Status**: Draft  
**Input**: User description: "Circuit Breaker & Multi-Provider Failover: Implement a deterministic 3-state Circuit Breaker (CLOSED, OPEN, HALF-OPEN) with jittered exponential backoff cooldown, automated zero-loss failover routing from Groq primary (Llama 3.3 70B) to OpenRouter fallback (Gemma-2-9B) upon HTTP 429 quota exhaustion or consecutive 5xx errors, and upstream HTTP/2 client adapters."

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Deterministic 3-State Circuit Breaker (Priority: P1) 🎯 MVP

As an API gateway operator, I want upstream provider health tracked by a deterministic 3-state circuit breaker (`CLOSED`, `OPEN`, `HALF-OPEN`) that trips immediately upon quota exhaustion or repeated server errors, so that traffic is not routed into failing or rate-limited external providers.

**Why this priority**: A faulty or rate-limited provider will block threads, exhaust connection pools, and accumulate request timeouts. A deterministic circuit breaker isolates failing providers immediately and prevents cascading system failures.

**Independent Test**:
1. Run requests against a primary provider.
2. Inject a simulated HTTP 429 (rate-limited/quota exhausted) response.
3. Verify the circuit breaker transitions instantaneously from `CLOSED` to `OPEN` without requiring subsequent request failures.
4. Verify all further requests immediately bypass the tripped provider.

**Acceptance Scenarios**:
1. **Given** the circuit breaker is in `CLOSED` state,  
   **When** an upstream provider returns an HTTP 429 quota-exhausted response,  
   **Then** the circuit breaker immediately trips to `OPEN`.
2. **Given** the circuit breaker is in `CLOSED` state,  
   **When** an upstream provider returns 3 consecutive HTTP 5xx errors or connection timeouts,  
   **Then** the circuit breaker trips to `OPEN`.
3. **Given** the circuit breaker is in `OPEN` state,  
   **When** subsequent requests arrive before the cooldown period has elapsed,  
   **Then** requests are prevented from calling the primary provider and diverted to fallback immediately.

---

### User Story 2 - Automated Multi-Provider Routing & Zero-Loss Failover (Priority: P1)

As a client microservice, I want queries normally destined for the primary provider (Groq / Llama 3.3 70B) to fail over automatically to the secondary fallback provider (OpenRouter / Gemma-2-9B) when the primary is unavailable or rate-limited, so that application traffic experiences 99.99% availability with zero dropped requests.

**Why this priority**: Microservices rely on the gateway to mask upstream provider outages. If Groq hits a quota ceiling or suffers downtime, the gateway must dynamically translate the request and dispatch it to OpenRouter without returning an error to the caller.

**Independent Test**:
1. Prime the system with normal traffic routed to Groq.
2. Inject an upstream 429 quota error on Groq during an active request.
3. Verify that the gateway intercepts the failure, retries the request transparently against OpenRouter, and returns an HTTP 200 completion to the client with `model: "google/gemma-2-9b-it"` and zero dropped calls.

**Acceptance Scenarios**:
1. **Given** a normal operational state with Groq circuit `CLOSED`,  
   **When** a client sends a chat completion request,  
   **Then** the gateway routes the request to Groq (`llama-3.3-70b-versatile`).
2. **Given** an in-flight request to Groq that receives an HTTP 429 or 5xx error,  
   **When** failover handling activates,  
   **Then** the gateway redirects the request to OpenRouter (`google/gemma-2-9b-it`), trips the Groq circuit breaker to `OPEN`, and returns the completion cleanly to the client.
3. **Given** Groq's circuit breaker is in `OPEN` state,  
   **When** new client requests arrive,  
   **Then** they are routed directly to OpenRouter without attempting Groq first.

---

### User Story 3 - Jittered Exponential Backoff Cooldown & Recovery Probing (Priority: P2)

As a systems engineer, I want the circuit breaker to use truncated exponential backoff with randomized jitter to govern probe intervals in `HALF-OPEN` state, so that recovered providers are safely verified without triggering thundering herd spikes.

**Why this priority**: When external providers recover from rate limits, slamming them with queued requests will immediately re-trip the rate limit. Jittered exponential backoff smooths probe traffic across gateway instances and guarantees gentle, phased recovery.

**Independent Test**:
1. Trip the primary circuit breaker to `OPEN`.
2. Observe that probe attempts do not fire until the randomized cooldown window expires.
3. Verify that after cooldown, the circuit transitions to `HALF-OPEN` and permits a single probe request.
4. If the probe succeeds, verify the circuit resets to `CLOSED`; if the probe fails, verify it re-enters `OPEN` with an increased backoff duration.

**Acceptance Scenarios**:
1. **Given** a circuit breaker in `OPEN` state,  
   **When** the cooldown timer with randomized jitter elapses,  
   **Then** the circuit transitions to `HALF-OPEN` and permits a trial request.
2. **Given** a circuit breaker in `HALF-OPEN` state,  
   **When** the probe request receives a successful HTTP 200 response,  
   **Then** the failure counter resets, the cooldown resets to base value, and the circuit transitions to `CLOSED`.
3. **Given** a circuit breaker in `HALF-OPEN` state,  
   **When** the probe request fails or returns an HTTP 429,  
   **Then** the circuit immediately re-trips to `OPEN` and doubles the backoff interval (up to the maximum configured ceiling).

---

### User Story 4 - Upstream Client Adapters & Chaos Failure Injection (Priority: P2)

As an integration test engineer, I want clean client abstractions for Groq and OpenRouter alongside a chaos mock client capable of simulating HTTP 429, 500, and timeout conditions, so that failover logic is fully verifiable offline without consuming paid upstream API credits.

**Why this priority**: Upstream APIs require network access and API credentials. A deterministic chaos mock ensures CI pipelines and local developers can simulate quota exhaustion, latency spikes, and server faults offline under sanitizers.

**Independent Test**:
1. Configure a chaos test harness with 100 simulated requests.
2. Inject a 429 quota exhaustion trigger at request #20.
3. Verify requests 1–19 hit the primary provider, request 20 fails over transparently, and requests 21–100 route directly to fallback with 100% aggregate success rate.

**Acceptance Scenarios**:
1. **Given** a configured upstream client adapter,  
   **When** a request is dispatched,  
   **Then** it formats the JSON payload conforming to provider expectations and parses the response choices and token usage.
2. **Given** a chaos test with fault injection active,  
   **When** requests are processed under high concurrency,  
   **Then** all fault conditions are handled gracefully without memory leaks or crashes.

---

### Edge Cases

- **Simultaneous Failure of Both Providers**: If both Groq and OpenRouter are unavailable or tripped to `OPEN`, the gateway must immediately return `StatusCode::UNAVAILABLE` with an informative error message rather than hanging or looping endlessly.
- **Model Parameter Adaptation**: When failing over from Groq (`llama-3.3-70b-versatile`) to OpenRouter (`google/gemma-2-9b-it`), model-specific configurations must adapt transparently (e.g., token limit adjustments, model name remapping).
- **Oscillating Recovery (Flapping Prevention)**: A single probe success in `HALF-OPEN` must be followed by stable operation; consecutive probe failures must exponentially penalize the cooldown interval up to the maximum ceiling (e.g. 60 seconds).
- **Thundering Herd during Probe**: When a circuit transitions to `HALF-OPEN`, only a single trial request must be permitted through; all other concurrent requests during the probe window must continue routing to fallback until the probe resolves.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST maintain an independent 3-state Circuit Breaker (`CLOSED`, `OPEN`, `HALF-OPEN`) for each configured upstream provider.
- **FR-002**: System MUST transition circuit to `OPEN` immediately upon receiving an HTTP 429 (Rate Limit / Quota Exhaustion) status from the upstream provider.
- **FR-003**: System MUST transition circuit to `OPEN` upon encountering a configurable number of consecutive failures (default: 3 consecutive 5xx errors or timeouts).
- **FR-004**: System MUST route normal chat completion traffic to the primary provider (Groq: `llama-3.3-70b-versatile`) when its circuit is `CLOSED`.
- **FR-005**: System MUST route traffic to the fallback provider (OpenRouter: `google/gemma-2-9b-it`) whenever the primary provider's circuit is `OPEN` or in a non-serving state.
- **FR-006**: System MUST execute zero-loss transparent failover: if an in-flight request to the primary provider fails with HTTP 429 or 5xx, the gateway must catch the failure and re-route the request to the fallback provider without returning an error to the caller.
- **FR-007**: System MUST implement truncated exponential backoff with full jitter to govern the cooldown interval before transitioning from `OPEN` to `HALF-OPEN`:
  $$\text{cooldown} = \text{random}(0, \min(\text{max\_backoff}, \text{base\_backoff} \times 2^{\text{consecutive\_failures}}))$$
- **FR-008**: System MUST permit exactly one trial probe request through when in `HALF-OPEN` state, while directing other concurrent requests to the fallback provider until the probe outcome is determined.
- **FR-009**: System MUST transition from `HALF-OPEN` to `CLOSED` upon a successful probe completion, resetting the failure counter and base backoff.
- **FR-010**: System MUST transition from `HALF-OPEN` back to `OPEN` upon a failed probe completion, incrementing the failure count and extending the backoff duration.
- **FR-011**: System MUST provide an abstract `IUpstreamClient` interface with standardized request execution, status translation, and response extraction.
- **FR-012**: System MUST provide a `MockUpstreamClient` capable of configurable fault injection (simulating HTTP 429, HTTP 500, timeouts, and success) for offline testing and CI.
- **FR-013**: System MUST correctly preserve correlation IDs and request tracking context across failover attempts.
- **FR-014**: System MUST return `StatusCode::UNAVAILABLE` if all configured providers are simultaneously tripped or unreachable.
- **FR-015**: System MUST log state transitions (`CLOSED` $\rightarrow$ `OPEN`, `OPEN` $\rightarrow$ `HALF-OPEN`, `HALF-OPEN` $\rightarrow$ `CLOSED`) with correlation and reason tags.

---

### Key Entities

- **CircuitState**: Enumeration representing provider availability: `CLOSED` (operational), `OPEN` (isolated), `HALF-OPEN` (probing).
- **CircuitBreakerConfig**: Configuration specifying consecutive failure threshold, base cooldown duration, maximum cooldown ceiling, and timeout limits.
- **UpstreamProvider**: Struct identifying provider configuration: provider name ("groq", "openrouter"), default model name, API endpoint, and timeout settings.
- **FailoverRouter**: Core routing service coordinating provider selection, circuit breaker evaluation, request translation, and failover retry dispatch.

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 0% dropped client requests during sudden upstream HTTP 429 rate-limit storms via automated zero-loss failover.
- **SC-002**: Failover routing decision and dispatch latency overhead is strictly $< 5\text{ms}$ above upstream response time.
- **SC-003**: Circuit breaker transitions from `CLOSED` to `OPEN` within 1 request upon receiving an HTTP 429 response.
- **SC-004**: In `HALF-OPEN` state, exactly 1 probe request is dispatched while concurrent requests continue to be served by fallback with zero race condition overages.
- **SC-005**: All failover and circuit breaker test targets achieve $\ge 90\%$ test coverage with zero memory leaks or undefined behavior under AddressSanitizer and UndefinedBehaviorSanitizer.
- **SC-006**: Gateway recovers smoothly to primary provider (`CLOSED`) once probe requests succeed without human intervention.

---

## Assumptions

- Groq API is the primary provider with model `llama-3.3-70b-versatile`; OpenRouter is the secondary fallback provider with model `google/gemma-2-9b-it`.
- Both upstream providers support OpenAI-compatible `/v1/chat/completions` JSON schemas.
- In offline and CI test environments, `MockUpstreamClient` simulates network responses without contacting external endpoints.
- Upstream authentication credentials (API keys) can be supplied via environment variables (`GROQ_API_KEY`, `OPENROUTER_API_KEY`) or CLI parameters.

