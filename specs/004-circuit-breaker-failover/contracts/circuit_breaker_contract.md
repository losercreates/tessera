# Interface Contracts: Circuit Breaker & Upstream Client Adapters

**Feature Branch**: `004-circuit-breaker-failover`  
**Date**: 2026-11-04  
**Status**: Completed  

---

## 1. C++ Core Interface: `ICircuitBreaker`

```cpp
#pragma once

#include "tessera/routing/circuit_types.hpp"
#include <memory>
#include <string_view>

namespace tessera {

class ICircuitBreaker {
 public:
  virtual ~ICircuitBreaker() = default;

  /**
   * Evaluates whether a request may be dispatched to the protected upstream.
   * - In CLOSED: returns true.
   * - In OPEN: returns false if cooldown window has not expired.
   * - In HALF-OPEN: atomically permits exactly ONE probe thread (returns true)
   *   while returning false to all concurrent callers.
   */
  [[nodiscard]] virtual bool CanAttempt() = 0;

  /**
   * Reports a successful upstream completion (HTTP 200).
   * Resets consecutive failures to 0 and transitions HALF-OPEN -> CLOSED.
   */
  virtual void RecordSuccess() = 0;

  /**
   * Reports an upstream execution failure (e.g. HTTP 429, 5xx, socket timeout).
   * - If status_code == 429: trips immediately to OPEN (Constitution Principle II).
   * - If consecutive failures >= threshold: trips to OPEN.
   * - If in HALF-OPEN: re-trips immediately to OPEN and doubles exponential backoff.
   */
  virtual void RecordFailure(int status_code, std::string_view error_message = "") = 0;

  /**
   * Returns current active circuit state (CLOSED, OPEN, HALF-OPEN).
   */
  [[nodiscard]] virtual CircuitState GetState() const noexcept = 0;

  /**
   * Retrieves atomic telemetry and operational statistics.
   */
  [[nodiscard]] virtual CircuitBreakerStats GetStats() const noexcept = 0;

  /**
   * Manually resets circuit to CLOSED and clears failure counters.
   */
  virtual void Reset() = 0;

  /**
   * Testing hook to inject or force a specific circuit state.
   */
  virtual void ForceState(CircuitState state) = 0;
};

}  // namespace tessera
```

---

## 2. Upstream Client Interface: `IUpstreamClient`

```cpp
#pragma once

#include "tessera/routing/circuit_types.hpp"
#include <string_view>

namespace tessera {

class IUpstreamClient {
 public:
  virtual ~IUpstreamClient() = default;

  /**
   * Executes an upstream chat completion request synchronously/asynchronously.
   * Formats OpenAI-compatible JSON payload, signs request with auth headers,
   * measures execution latency, and parses choice texts and token usage.
   */
  [[nodiscard]] virtual UpstreamResponse Execute(const UpstreamRequest& request) = 0;

  /**
   * Identifier of the provider (e.g. "groq", "openrouter", "mock").
   */
  [[nodiscard]] virtual std::string_view ProviderName() const noexcept = 0;

  /**
   * Configured API base endpoint URL.
   */
  [[nodiscard]] virtual std::string_view Endpoint() const noexcept = 0;

  /**
   * Default model served by this client adapter.
   */
  [[nodiscard]] virtual std::string_view DefaultModel() const noexcept = 0;
};

}  // namespace tessera
```

---

## 3. Resilient Failover Router: `FailoverRouter`

```cpp
#pragma once

#include "tessera/backend/completion_backend.hpp"
#include "tessera/routing/circuit_breaker.hpp"
#include "tessera/backend/upstream_client.hpp"
#include <memory>

namespace tessera {

class FailoverRouter : public ICompletionBackend {
 public:
  FailoverRouter(
      std::shared_ptr<IUpstreamClient> primary_client,
      std::shared_ptr<ICircuitBreaker> primary_breaker,
      std::shared_ptr<IUpstreamClient> fallback_client,
      std::shared_ptr<ICircuitBreaker> fallback_breaker,
      std::string fallback_model = "google/gemma-2-9b-it");

  /**
   * Implements ICompletionBackend::Complete.
   * Coordinates primary execution, circuit breaker checks, transparent retry
   * upon 429/5xx, model translation, and error escalation.
   */
  Status Complete(const ChatCompletionRequest& request, ChatCompletionResponse* response) override;

  [[nodiscard]] std::string_view Name() const noexcept override {
    return "FailoverRouter";
  }

  [[nodiscard]] std::shared_ptr<ICircuitBreaker> PrimaryBreaker() const noexcept {
    return primary_breaker_;
  }

  [[nodiscard]] std::shared_ptr<ICircuitBreaker> FallbackBreaker() const noexcept {
    return fallback_breaker_;
  }

  [[nodiscard]] FailoverRouterStats GetStats() const noexcept;

 private:
  std::shared_ptr<IUpstreamClient> primary_client_;
  std::shared_ptr<ICircuitBreaker> primary_breaker_;
  std::shared_ptr<IUpstreamClient> fallback_client_;
  std::shared_ptr<ICircuitBreaker> fallback_breaker_;
  std::string fallback_model_;
};

}  // namespace tessera
```

---

## 4. Upstream Error Code & gRPC Status Code Mappings

| Upstream HTTP Status | Circuit Action | Failover Action | Client-Facing Gateway gRPC Status |
|---|---|---|---|
| `200 OK` | `RecordSuccess()` | None (Success returned) | `StatusCode::OK` |
| `429 Too Many Requests` | Immediate Trip to `OPEN` | Transparent retry on Fallback | Transparent: `StatusCode::OK` via Fallback |
| `500 Internal Server Error` | Increment failures; trip if $\ge 3$ | Transparent retry on Fallback | Transparent: `StatusCode::OK` via Fallback |
| `502 / 503 / 504 Gateway Error` | Increment failures; trip if $\ge 3$ | Transparent retry on Fallback | Transparent: `StatusCode::OK` via Fallback |
| Socket Timeout (0) | Increment failures; trip if $\ge 3$ | Transparent retry on Fallback | Transparent: `StatusCode::OK` via Fallback |
| Both Providers Tripped/Failing | Record failures on both | None available | `StatusCode::UNAVAILABLE` ("All upstream providers exhausted") |
| Invalid Client Payload | No circuit impact | None (client error) | `StatusCode::INVALID_ARGUMENT` |

