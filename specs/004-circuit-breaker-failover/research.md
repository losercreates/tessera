# Research & Architectural Decisions: Circuit Breaker & Multi-Provider Failover

**Feature Branch**: `004-circuit-breaker-failover`  
**Date**: 2026-11-04  
**Status**: Completed  

---

## 1. Deterministic 3-State Circuit Breaker State Machine

### Decision
Implement a deterministic, lock-free/low-contention 3-state Circuit Breaker (`CircuitBreaker`) tracking `CLOSED`, `OPEN`, and `HALF-OPEN` states with:
- Instant trip from `CLOSED` to `OPEN` on a single HTTP 429 (Rate Limit / Quota Exhaustion) status.
- Trip from `CLOSED` to `OPEN` on reaching a configurable threshold of consecutive failures (default: 3 consecutive 5xx errors or connection timeouts).
- Atomic transition to `HALF-OPEN` when the jittered cooldown duration expires.
- Strict single-probe gating in `HALF-OPEN`: exactly one trial request is permitted to probe the primary upstream; any concurrent requests arriving while probing is in progress are immediately diverted to the fallback provider without waiting.
- If the trial probe succeeds (HTTP 200), transition to `CLOSED`, resetting consecutive failure count to 0 and backoff to base.
- If the trial probe fails (429, 5xx, timeout), transition back to `OPEN`, increment consecutive failure count, and compute the next jittered exponential backoff.

### Rationale
- High-throughput gateways handling thousands of requests per second cannot afford heavy global mutex locks across every request. Using atomic state variables (`std::atomic<CircuitState>`, `std::atomic<uint32_t> consecutive_failures_`, `std::atomic<std::chrono::steady_clock::time_point> open_until_`) with a mutex or compare-and-swap (CAS) probe reservation (`std::atomic<bool> probe_in_flight_`) ensures thread-safety under hundreds of concurrent threads.
- Instant trip on HTTP 429 is a non-negotiable constitution requirement (Principle II). Quota exhaustion will not self-heal on the very next millisecond; continuing to send requests exhausts upstream rate limit quotas further and incurs wasted network latency.
- Diverting concurrent traffic during `HALF-OPEN` prevents thundering herds from overwhelming a recovering LLM provider.

### Alternatives Considered
- **Sliding Window Error Rate (e.g. Netflix Hystrix / Resilience4j)**: Evaluates failure percentage over a rolling 10-second window. Rejected for HTTP 429 handling because waiting for a percentage threshold would allow dozens of requests to fail before tripping; Constitution Principle II requires instantaneous trip on HTTP 429. Consecutive error counting with instant 429 trip provides superior determinism and sub-millisecond trip latency.
- **2-State Circuit Breaker (OPEN / CLOSED only)**: Without `HALF-OPEN`, traffic must switch all-or-nothing back to the primary provider when the timer expires. If the provider is still degraded, all incoming requests would hit the failing provider simultaneously. Rejected.

---

## 2. Truncated Exponential Backoff with Full Randomized Jitter

### Decision
Adopt the AWS Architecture Full Jitter algorithm for calculating the cooldown duration of the `OPEN` state:
$$\text{calculated\_backoff} = \min(\text{max\_cooldown}, \text{base\_cooldown} \times 2^{\text{consecutive\_failures}})$$
$$\text{cooldown} = \text{random\_uniform}(0, \text{calculated\_backoff})$$

Default parameters:
- `base_cooldown`: 1,000 ms (1 second)
- `max_cooldown`: 60,000 ms (60 seconds)
- `consecutive_failure_threshold`: 3 failures (for 5xx / timeouts; 429 trips immediately)
- Thread-safe pseudo-random generator: `thread_local std::mt19937_64` seeded with high-resolution clock and thread ID.

### Rationale
- In distributed environments and multi-threaded gateways, fixed exponential backoff causes synchronized retries ("thundering herds") where multiple threads/instances wake up simultaneously and hammer the upstream provider, immediately forcing it back into rate limiting.
- Full jitter spreads probe attempts uniformly across the interval $[0, \text{calculated\_backoff}]$, minimizing peak concurrent connection spikes against Groq and OpenRouter.
- Truncating at `max_cooldown` (60s) ensures the gateway does not back off indefinitely into hours during prolonged outages, maintaining active probe recovery once the provider becomes healthy again.

### Alternatives Considered
- **Fixed Linear Backoff (e.g. constant 10s cooldown)**: Ineffective under sustained rate-limit conditions, leading to repetitive synchronized failures every 10 seconds.
- **Equal Jitter ($\frac{B}{2} + \text{random}(0, \frac{B}{2})$)**: Guarantees a minimum sleep of half the backoff, but still creates clustering around the midpoint. Full Jitter exhibits the lowest aggregate collision rate across distributed benchmark tests.
- **Decorrelated Jitter**: Maintains a running sleep state $S_{i+1} = \min(M, \text{random}(B, S_i \times 3))$. Adds unnecessary statefulness across independent probe cycles; Full Jitter is simpler, stateless with respect to failure count, and deterministic.

---

## 3. Automated Zero-Loss Failover Routing Architecture

### Decision
Implement `FailoverRouter` as an implementation of `ICompletionBackend` that orchestrates:
1. Primary Provider: Groq (`llama-3.3-70b-versatile`) with its own `CircuitBreaker`.
2. Fallback Provider: OpenRouter (`google/gemma-2-9b-it`) with its own `CircuitBreaker`.
3. Execution Lifecycle:
   - When request arrives, inspect Primary `CircuitBreaker.CanAttempt()`.
   - If allowed: attempt Primary upstream.
     - On Success (HTTP 200): Record success on Primary `CircuitBreaker`. Return completion.
     - On Failure (HTTP 429, 5xx, Timeout): Record failure on Primary `CircuitBreaker` (which trips it to `OPEN` if 429 or 3rd consecutive 5xx). Transparently redirect the exact request to Fallback upstream (OpenRouter) with model adaptation.
   - If Primary is `OPEN`: Route directly to Fallback upstream without attempting Primary.
   - If Fallback succeeds: Record success on Fallback `CircuitBreaker`. Return completion with metadata indicating fallback execution.
   - If Fallback also fails: Record failure on Fallback `CircuitBreaker`. Return `StatusCode::UNAVAILABLE` with clear diagnostic details.
4. Model Adaptation:
   - When failing over, dynamically remap model name from `llama-3.3-70b-versatile` to `google/gemma-2-9b-it` (or user-configured fallback model).
   - Preserve all prompt messages, temperature, max_tokens, correlation ID, and metadata.

### Rationale
- Zero-loss failover ensures calling microservices experience 99.99% availability even when the primary LLM provider suffers an outage or exhausts its rate limit tier.
- By implementing `ICompletionBackend`, `FailoverRouter` seamlessly slots into Tessera's existing decorator pipeline:
  `GatewayServer` $\rightarrow$ `RateLimitedBackend` $\rightarrow$ `CachedCompletionBackend` $\rightarrow$ `FailoverRouter` $\rightarrow$ Upstream Providers.
- Cache population on miss happens naturally because `CachedCompletionBackend` intercepts the successful response returned by `FailoverRouter` (whether from primary or fallback) and stores it in Tier 1 and Tier 2 caches.

### Alternatives Considered
- **Client-Side Failover**: Requiring calling microservices to catch 429/500 and manually query a fallback gateway. Rejected because it leaks upstream operational topology into microservice business logic and invalidates gateway SLAs.
- **Parallel Hedged Requests**: Sending requests simultaneously to Groq and OpenRouter and returning the fastest. Rejected because it doubles token costs and consumes scarce daily request quotas (Groq: 1,000 RPD, OpenRouter: 50 RPD) under normal conditions.

---

## 4. Upstream Client Adapters & Chaos Fault Injection

### Decision
Define a clean, decoupled abstraction:
1. `IUpstreamClient`: Abstract base class providing `virtual UpstreamResponse Execute(const UpstreamRequest& req) = 0`.
2. `UpstreamRequest` / `UpstreamResponse`: Structured representations containing HTTP status codes, headers, body strings, parsed choices, usage metrics, and latency duration.
3. `MockUpstreamClient`: In-memory chaos test client with deterministic scriptable behaviors:
   - Configurable response queues per provider (e.g. sequence of `[200, 200, 429, 500, 200]`).
   - Latency simulation / artificial delay.
   - Deterministic 429 quota exhaustion injection, 503 service unavailable, socket timeout simulation, and malformed payload injection.
   - Offline verification without network sockets, API keys, or cloud charges.
4. `HttpUpstreamClient`: Standard production HTTP client adapter implementing `IUpstreamClient` with TLS support via OpenSSL, connection pooling, and OpenAI-compatible `/v1/chat/completions` request dispatch.

### Rationale
- Enables hermetic, 100% reproducible unit and integration tests inside isolated environments and CI pipelines (which have no external internet access).
- Facilitates chaos testing under AddressSanitizer and UndefinedBehaviorSanitizer to guarantee zero memory leaks or data races under rapid failovers and high concurrency.

### Alternatives Considered
- **Spawning External Python/Node Mock Server Processes**: Fragile, slow startup, port collision risks in CI parallel runs, and complex lifecycle management. In-memory `MockUpstreamClient` executes in sub-microseconds with zero external dependencies.
- **Heavy libcurl dependency**: In environments where libcurl is not pre-installed or headers vary across distributions, relying solely on libcurl breaks hermetic builds. A clean interface allowing in-memory mocks alongside an OpenSSL-based client provides maximum portability.

---

## 5. Thread Safety & Concurrency Model

### Decision
- `CircuitBreaker` uses a hybrid concurrency model:
  - Atomic fast-path checks for `CLOSED` and `OPEN` states (`std::memory_order_acquire` / `std::memory_order_release`).
  - `probe_in_flight_` atomic flag (`std::atomic<bool>`) using `compare_exchange_strong` to guarantee that exactly one thread acquires the right to issue the probe request when the circuit enters `HALF-OPEN`.
  - Concurrency overhead per check is $< 50\text{ns}$, well within the $< 5\text{ms}$ failover overhead budget (SC-002).
- `FailoverRouter` is stateless beyond holding shared pointers to thread-safe `CircuitBreaker` instances and upstream clients, enabling concurrent execution across all worker threads.

### Rationale
- Ensures no deadlocks, no priority inversion, and minimal lock contention under 200+ concurrent worker threads.

