# Data Model: Provider-Agnostic Routing & Dynamic Model Cascade

**Feature**: `007-provider-agnostic-routing`  
**Date**: 2026-11-19  
**Status**: Completed  

---

## 1. Domain Entities & Relationships

```mermaid
classDiagram
    class ProviderConfig {
        +string name
        +string endpoint
        +string api_key
        +string default_model
        +vector~string~ supported_models
        +CircuitBreakerConfig circuit_config
        +vector~RateLimitRule~ rate_limits
        +bool IsLocal()
    }

    class CascadeTarget {
        +shared_ptr~IUpstreamClient~ client
        +shared_ptr~ICircuitBreaker~ breaker
        +string target_model
        +uint32 priority
    }

    class CascadeRoute {
        +string route_id
        +string description
        +vector~CascadeTarget~ targets
        +size_t TargetCount()
    }

    class ModelRoutingRule {
        +string pattern
        +string target_provider
        +string target_model
        +shared_ptr~CascadeRoute~ cascade
        +bool Matches(string model)
    }

    class ProviderRegistry {
        +RegisterProvider(ProviderConfig config)
        +RegisterRoute(string model, CascadeRoute route)
        +ResolveCascade(string model) CascadeRoute
        +GetProvider(string name) shared_ptr~IUpstreamClient~
    }

    ProviderRegistry "1" *-- "many" ProviderConfig : catalogs
    ProviderRegistry "1" *-- "many" ModelRoutingRule : evaluates
    ModelRoutingRule "1" o-- "1" CascadeRoute : references
    CascadeRoute "1" *-- "many" CascadeTarget : executes sequentially
```

---

## 2. Entity Specifications

### 2.1 ProviderConfig
Represents the static declaration of an upstream AI service.

| Field | Type | Required | Description | Validation Rules |
| :--- | :--- | :---: | :--- | :--- |
| `name` | `std::string` | Yes | Unique provider identifier (e.g. `groq`, `deepseek`, `ollama`) | Alphanumeric + hyphens, lowercase, non-empty |
| `endpoint` | `std::string` | Yes | Upstream HTTP/HTTPS base URL | Must start with `http://` or `https://` |
| `api_key` | `std::string` | No | Upstream API bearer secret | Optional (empty for local runtimes like Ollama/vLLM) |
| `default_model` | `std::string` | Yes | Canonical model identifier served by this provider | Non-empty string |
| `supported_models` | `std::vector<std::string>` | No | List of additional models available on this provider | Non-empty if specified |
| `circuit_config` | `CircuitBreakerConfig` | No | Failure threshold, base cooldown, max cooldown | Positive integers |
| `rate_limits` | `std::vector<RateLimitRule>` | No | Per-window request and token bucket constraints | Valid capacities and refill rates |

### 2.2 CascadeTarget
Represents an individual stage within an $N$-tier fallback sequence.

| Field | Type | Description |
| :--- | :--- | :--- |
| `client` | `std::shared_ptr<IUpstreamClient>` | Executable HTTP client pointing to the provider endpoint |
| `breaker` | `std::shared_ptr<ICircuitBreaker>` | Dedicated 3-state circuit breaker governing this provider stage |
| `target_model` | `std::string` | Target model name to forward to this provider stage (overriding original model if needed) |
| `priority` | `uint32_t` | Sequence rank (0 = primary, 1 = secondary, etc.) |

### 2.3 CascadeRoute
An ordered sequence of fallback targets evaluated progressively until one succeeds.

| Field | Type | Description |
| :--- | :--- | :--- |
| `route_id` | `std::string` | Model pattern or route name (e.g., `default`, `deepseek-chat`, `coding-cascade`) |
| `targets` | `std::vector<CascadeTarget>` | Non-empty ordered sequence of candidate targets |

### 2.4 ModelRoutingRule
Defines how incoming model requests map to cascade routes.

| Field | Type | Description |
| :--- | :--- | :--- |
| `pattern` | `std::string` | Exact model name (`deepseek-chat`) or wildcard prefix (`ollama/*`, `groq/*`) |
| `cascade` | `std::shared_ptr<CascadeRoute>` | Target cascade route executed when pattern matches |

---

## 3. State Machine & Lifecycle Transitions

### 3.1 Per-Provider Circuit Breaker State Transitions
Each provider independently transitions through the standard 3-state lifecycle:

```mermaid
stateDiagram-v2
    [*] --> CLOSED : Gateway Initialization
    CLOSED --> OPEN : HTTP 429 Quota Exhausted OR 3 Consecutive 5xx Errors
    OPEN --> HALF_OPEN : Randomized Jitter Cooldown Elapsed
    HALF_OPEN --> CLOSED : Trial Probe Request Succeeds (HTTP 200)
    HALF_OPEN --> OPEN : Trial Probe Fails (HTTP 429 or 5xx) -> Cooldown Multiplied
```

### 3.2 Request Routing Flow
When an incoming `ChatCompletionRequest` is received:

```mermaid
flowchart TD
    Req[ChatCompletionRequest] --> CacheCheck{Cache Hit?}
    CacheCheck -- Tier 1 Exact Hit --> ReturnExact[Return SHA-256 Exact Cached Response]
    CacheCheck -- Tier 2 Semantic Hit --> ReturnSemantic[Return Vector Semantic Cached Response]
    CacheCheck -- Miss --> RouteResolver[Resolve Target Model & CascadeRoute]

    RouteResolver --> Target1["Target 1 (Primary)"]
    Target1 -- Breaker OPEN --> Target2["Target 2 (Secondary)"]
    Target1 -- Breaker CLOSED --> Exec1["client->Execute(T1)"]
    Exec1 -- Success 200 --> CacheIndex["Asynchronously Index in Caches & Return 200"]
    Exec1 -- Fail 429/5xx --> Trip1["Trip T1 Breaker to OPEN"] --> Target2

    Target2 -- Breaker OPEN --> TargetN["Target N (Tertiary / Fallback)"]
    Target2 -- Breaker CLOSED --> Exec2["client->Execute(T2)"]
    Exec2 -- Success 200 --> CacheIndex
    Exec2 -- Fail 429/5xx --> Trip2["Trip T2 Breaker to OPEN"] --> TargetN

    TargetN -- Breaker CLOSED --> ExecN["client->Execute(TN)"]
    ExecN -- Success 200 --> CacheIndex
    ExecN -- Fail --> ErrAll["Return HTTP 503: All Cascade Providers Exhausted"]
    TargetN -- Breaker OPEN --> ErrAll
```
