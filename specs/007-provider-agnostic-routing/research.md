# Research & Architecture Decisions: Provider-Agnostic Routing & Dynamic Model Cascade

**Feature**: `007-provider-agnostic-routing`  
**Date**: 2026-11-19  
**Status**: Completed  

---

## 1. Provider Registry & Upstream Client Abstraction

### Context & Challenge
The gateway previously hardcoded `groq` and `openrouter` instances in `src/main.cpp` and `FailoverRouter`. To support arbitrary platforms (OpenAI, DeepSeek, Together AI, Mistral, and local clusters like Ollama/vLLM) without code modification or recompilation, the gateway requires an extensible catalog managing provider lifecycles.

### Decision
Implement `ProviderRegistry` as an in-memory catalog holding configured `ProviderInstance` entities:
* Each instance bundles:
  1. `std::shared_ptr<IUpstreamClient>` (configured with endpoint URL, default model, and optional API key).
  2. `std::shared_ptr<ICircuitBreaker>` (dedicated 3-state state machine with jittered exponential backoff).
  3. `std::vector<RateLimitRule>` (per-provider token-bucket capacity and refill rates).
  4. Metadata (provider name, supported models, operational status).
* `HttpUpstreamClient` remains the universal transport adapter. Because standard cloud and local engines (Ollama, vLLM, DeepSeek, OpenAI, Groq, OpenRouter) adhere to the OpenAI `/v1/chat/completions` REST wire protocol, `HttpUpstreamClient` communicates with any of them out-of-the-box over OpenSSL TLS or cleartext HTTP without per-vendor subclasses.
* When a provider configuration has an empty API key (e.g. local Ollama at `http://localhost:11434/v1/chat/completions`), the adapter automatically skips adding an `Authorization` header.

### Rationale
* **Zero Vendor Lock-in**: Adding a new provider is purely a configuration action.
* **Complete Isolation**: Each provider possesses its own circuit breaker and rate limit state, preventing outages on one service from affecting others.
* **Reuse of Hardened Components**: Leverages the existing ASan/UBSan-clean `HttpUpstreamClient`, `CircuitBreaker`, and `DistributedRateLimiter`.

### Alternatives Considered
1. *Vendor-specific client subclasses (`GroqClient`, `DeepSeekClient`, `OllamaClient`)*: Rejected because the wire protocol is 100% identical (OpenAI chat completions standard). Adding specialized subclasses introduces duplicate code without functional benefit.
2. *Dynamic plugin `.so` loading*: Rejected due to unnecessary runtime complexity, potential ABI incompatibility across compilers, and sanitizer verification hazards.

---

## 2. $N$-Tier Priority Fallback Cascade Architecture

### Context & Challenge
`FailoverRouter` previously enforced a rigid binary tuple: `primary_client_` and `fallback_client_`. Real-world production architectures require $N$-tier cascades (e.g., Local Ollama $\rightarrow$ High-Speed Groq $\rightarrow$ DeepSeek $\rightarrow$ OpenRouter $\rightarrow$ OpenAI) to maximize cost savings and guarantee 99.99% availability.

### Decision
Introduce `CascadeRouter` (implementing `ICompletionBackend`) that accepts an ordered sequence of cascade targets (`std::vector<CascadeTarget>`):
* Each `CascadeTarget` specifies:
  * Pointer to the target provider.
  * Target model override (optional, e.g. mapping to `openrouter/free` if failing over to an aggregator).
* Execution algorithm:
  1. Iterate sequentially through candidate targets: $T_1, T_2, \dots, T_N$.
  2. For candidate $T_i$, inspect its dedicated circuit breaker (`breaker->CanAttempt()`). If `OPEN`, record direct bypass and advance immediately to $T_{i+1}$.
  3. If `CanAttempt()` is true, dispatch `client->Execute(req)`.
  4. If response is success (HTTP 2xx), record breaker success, record telemetry latency, and return immediately.
  5. If response is failure (HTTP 429, 5xx, socket timeout), trip $T_i$'s breaker to `OPEN`, record failure metrics, and advance transparently to $T_{i+1}$.
  6. If all $N$ candidates are exhausted without success, return `Status::Unavailable("All upstream providers in cascade exhausted")`.
* Maintain backward compatibility: Refactor `FailoverRouter` to inherit from or wrap `CascadeRouter` with a 2-target constructor so existing integration and unit tests pass with zero breakage.

### Rationale
* **Arbitrary Depth**: Supports 1, 2, 3, or $N$ fallback levels without architectural limits.
* **Zero Dropped Requests**: Client requests transparently navigate around failing tiers in sub-second times.
* **Graceful Degradation**: Traffic cascades from low-cost / low-latency tiers to fallback tiers seamlessly.

### Alternatives Considered
1. *Concurrent speculative execution (hedged requests)*: Firing requests to multiple providers simultaneously and picking the fastest. Rejected because it multiplies token costs and quota consumption by $N$.
2. *Round-robin load balancing*: Rejected because upstream providers have vastly different cost profiles (e.g., free local Ollama vs paid cloud APIs); deterministic priority cascade is required for cost and quota governance.

---

## 3. Dynamic Model-Based Routing & Alias Resolution

### Context & Challenge
Clients want to send `model: "deepseek-chat"` or `model: "llama-3.1-8b-instant"` and have the gateway intelligently route to the optimal backend without client-side endpoint logic.

### Decision
Implement `ModelRouter` with two-tier model resolution:
1. **Explicit Exact Mapping**: Lookup table mapping exact model identifiers to specific provider cascades:
   * e.g., `"deepseek-chat"` $\rightarrow$ Cascade: `[deepseek, openrouter]`
   * e.g., `"llama-3.1-8b-instant"` $\rightarrow$ Cascade: `[groq, ollama, openrouter]`
2. **Provider-Prefixed Wildcard Matching**:
   * e.g., `"ollama/*"` $\rightarrow$ route directly to provider `ollama` with prefix stripped.
   * e.g., `"groq/*"` $\rightarrow$ route directly to provider `groq` with prefix stripped.
3. **Default Catch-All Cascade**:
   * If a model is empty or not explicitly mapped, dispatch to the default system cascade.
4. **Validation & Discovery**:
   * Provide a method to query all registered models for error messaging and client discovery.

### Rationale
* **Unified Single API**: Clients consume all models across all vendors through one gateway port.
* **Operational Control**: Operators can redirect a model alias (e.g., `"coding-assistant"`) from one vendor to another without client redeployment.

### Alternatives Considered
1. *Regex-only routing engine*: Rejected due to latency overhead and edge-case backtracking risks. Hash-table lookup + simple prefix matching provides $O(1)$ sub-microsecond resolution.

---

## 4. Declarative Provider Configuration (Environment & Structured Format)

### Context & Challenge
Tessera must be deployable in containerized Docker/Kubernetes environments where configuration is provided via environment variables (`.env`) or structured configuration files (`tessera.json` / `tessera.yaml`).

### Decision
Support a dual-mode declarative configuration parser (`ProviderConfigLoader`):
1. **Structured JSON/File Configuration**:
   * Allows full specification of providers, endpoints, keys, models, circuit configs, and cascade routes.
2. **Multi-Provider Environment Variable Conventions**:
   * Enables plug-and-play configuration in Docker Compose via numbered or named env variables:
     * `TESSERA_PROVIDERS=groq,openrouter,deepseek,ollama`
     * `PROVIDER_GROQ_ENDPOINT=https://api.groq.com/openai/v1/chat/completions`
     * `PROVIDER_GROQ_KEY=gsk_...`
     * `PROVIDER_GROQ_MODEL=llama-3.1-8b-instant`
     * `PROVIDER_OLLAMA_ENDPOINT=http://localhost:11434/v1/chat/completions`
     * `PROVIDER_OLLAMA_MODEL=llama3.2`
3. **Automatic Fallback Defaults**:
   * If no external provider configuration is supplied, automatically instantiate the canonical baseline providers (Groq primary $\rightarrow$ OpenRouter fallback) ensuring out-of-the-box operation and CI reproducibility.

### Rationale
* **Cloud-Native Compatibility**: Adheres to Twelve-Factor App methodology for environment-based configuration.
* **Zero Breaking Changes**: Existing `.env` configurations continue to work transparently.

---

## 5. Dynamic Telemetry & Metric Attribution

### Context & Challenge
`TelemetryRegistry` previously pre-seeded gauges for `"groq"` and `"openrouter"`. Adding arbitrary providers must dynamically initialize and track metrics without race conditions or memory leaks.

### Decision
Update `TelemetryRegistry` to dynamically register metric streams on first observation using thread-safe map lookups:
* Counters: `gateway_requests_total{model="...", provider="...", status="..."}` dynamically tracks any provider string.
* Gauges: `circuit_breaker_state{provider="..."}` dynamically instantiates an atomic gauge on demand.
* Histograms: `upstream_latency_ms{provider="..."}` dynamically instantiates a thread-safe histogram for any observed provider.
* Export: Standard Prometheus 0.0.4 text output iterates through all dynamically registered providers.

### Rationale
* **Full Observability**: Every new provider automatically appears in Prometheus scrapes and Grafana dashboards with zero manual metric registration code.
* **Thread Safety**: Uses atomic counters and mutex-protected registry insertion with double-checked read locks.
