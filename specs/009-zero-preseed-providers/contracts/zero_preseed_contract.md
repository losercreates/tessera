# Interface & CLI Contracts: Zero-Preseed Provider Limiter & Telemetry Initialization

**Feature**: `009-zero-preseed-providers`  
**Contract Version**: 1.0.0  
**Scope**: `DistributedRateLimiter`, `TelemetryRegistry`, `ProviderRegistry`, `ModelRouter`, `gateway_client`

---

## 1. `DistributedRateLimiter` Initialization & Opt-In Contract

```cpp
// include/tessera/ratelimit/rate_limiter.hpp
class DistributedRateLimiter {
 public:
  explicit DistributedRateLimiter(
      std::shared_ptr<RedisConnectionPool> redis_pool,
      bool load_standard_policies = false);

  void AddRule(RateLimitRule rule);
  void LoadStandardPolicies();
  void ClearRules(const std::string& provider);
  void ClearAllRules();
  ...
};
```

### Invariants
1. When constructed with `load_standard_policies = false` (default), `rules_by_provider_` is initialized completely empty (`HasRules("groq") == false`, `HasRules("openrouter") == false`).
2. Calling `LoadStandardPolicies()` explicitly seeds `RateLimitPolicies::GroqRules()` and `RateLimitPolicies::OpenRouterRules()` idempotently via `AddRule()`.
3. Calling `ClearAllRules()` empties `rules_by_provider_` completely and does NOT re-seed any default rules.
4. `CheckAndConsume(provider, tokens)` continues returning `allowed = true` when no rules are configured for `provider`.

---

## 2. `TelemetryRegistry` Initialization, Reset & Opt-In Contract

```cpp
// include/tessera/telemetry/telemetry_registry.hpp
class TelemetryRegistry {
 public:
  ...
  void RegisterProvider(std::string_view provider);
  void RegisterStandardProviders();  // Explicit opt-in for legacy unit tests ("groq", "openrouter")
  ...
  void Reset();
};
```

### Invariants
1. Default construction initializes `requests_counters_`, `ratelimit_skips_counters_`, `circuit_gauges_`, and `latency_histograms_` empty, and zero-initializes cache hit/miss counters.
2. `Reset()` clears all four provider-indexed maps completely (`GetTrackedProviders().empty() == true`) without re-inserting `"groq"` or `"openrouter"`.
3. `RegisterStandardProviders()` explicitly invokes `RegisterProvider("groq")` and `RegisterProvider("openrouter")` ONLY when opted into by tests or callers that require pre-seeded legacy series.
4. Read-only accessors (`GetCircuitBreakerState`, `GetUpstreamLatencySnapshot`, `GetRequestsTotal`, `GetRateLimitSkipsTotal`) MUST remain `const` and MUST NOT insert missing keys into internal maps.
5. Write accessors (`IncrementRequestsTotal`, `SetCircuitBreakerState`, `RecordUpstreamLatency`, `IncrementRateLimitSkipsTotal`) retain lazy per-provider entry creation for active runtime observations.
6. `ToPrometheusFormat()` always emits metric `# HELP` / `# TYPE` headers and `gateway_cache_hits_total`, and emits provider-labeled rows (`gateway_requests_total`, `circuit_breaker_state`, `upstream_latency_ms_*`, `gateway_ratelimit_skips_total`) solely for tracked providers.

---

## 3. `ProviderRegistry` & `ModelRouter` Factory Extensions

```cpp
// include/tessera/routing/provider_registry.hpp
class ProviderRegistry {
 public:
  ...
  static ProviderRegistry LoadFromEnvironment(
      bool preseed_defaults = true,
      ClientFactory custom_client_factory = nullptr);
};

// include/tessera/routing/model_router.hpp
class ModelRouter : public ICompletionBackend {
 public:
  ...
  [[nodiscard]] std::shared_ptr<CascadeRouter> GetDefaultCascade() const;
};
```

### Invariants
1. `LoadFromEnvironment(preseed_defaults, custom_client_factory)`:
   - Backward-compatible default arguments (`preseed_defaults = true`, `custom_client_factory = nullptr`).
   - If `custom_client_factory` is non-null, calls `registry.SetClientFactory(std::move(custom_client_factory))` before parsing env variables and registering any providers.
   - Registers zero providers when `preseed_defaults == false` and no explicit provider env vars are configured.
2. `ModelRouter::GetDefaultCascade()` returns the configured `default_cascade_` (`nullptr` if none) thread-safely so callers (`gateway_client`, diagnostics, tests) can read aggregate cascade stats cleanly.

---

## 4. `gateway_client` CLI Pipeline & Output Contract

- **Pipeline Wiring**:
  - `CachedCompletionBackend` wrapping `ModelRouter::CreateFromRegistry(provider_registry, rate_limiter)` (no outer `RateLimitedBackend`, no hardcoded `FailoverRouter`).
  - `rate_limiter = std::make_shared<DistributedRateLimiter>(redis_pool)` followed by `provider_registry.RegisterRateLimits(*rate_limiter)`.
  - Uses `ClientFactory` choosing live `HttpUpstreamClient` when `(!cfg.api_key.empty() || live_mode) && !failover_demo` (via optional `--live` CLI flag for unkeyed local daemons), and offline `MockUpstreamClient` otherwise (enqueuing simulated `429` on Tier 0 when `--failover-demo` is enabled).
- **Default Request Model & Single-Tier Failover Guard**:
  - If `--model=<name>` is passed explicitly, uses `<name>`.
  - Otherwise, if `--failover-demo` is enabled:
    - If `provider_registry.ProviderCount() >= 2`, selects `"auto"` so `ModelRouter` routes through `default_cascade_` (exercising multi-tier failover from Tier 0 to Tier 1 rather than pinning an exact single-provider route).
    - If `provider_registry.ProviderCount() == 1` (e.g., `--groq-key` alone without custom providers or a single-entry `TESSERA_PROVIDERS`), logs a warning clearly that no fallback tier is available in `--failover-demo` mode and proceeds through the configured route cleanly.
  - Otherwise, uses the first registered provider's `default_model` (fallback `"auto"`).
- **Diagnostics Output**:
  - Prints completion fields cleanly, `Upstream Mode` reflecting active client mode, dynamic cascade telemetry from `model_router->GetDefaultCascade()->GetStats()` (`rate_limit_skips`, `failover_attempts`, `direct_bypasses`, `successful_requests`), and per-provider circuit breaker states for every provider in `provider_registry.GetRegisteredProviders()`.
- **Prometheus Verification Compatibility (`scripts/verify_metrics_format.sh`)**:
  - Invoking `./build/gateway_client --message "Metrics validation test" --metrics` in clean env passes all assertions in `scripts/verify_metrics_format.sh`.
  - Setting `TESSERA_PROVIDERS=ollama,deepseek` (or indexed `PROVIDER_*` env vars) results in `./build/gateway_client --message "test" --metrics` emitting metrics strictly for configured providers (`ollama`, `deepseek`) with zero phantom `groq` or `openrouter` lines.
