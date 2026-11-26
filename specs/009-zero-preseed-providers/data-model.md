# Data Model: Zero-Preseed Provider Limiter & Telemetry Initialization

**Feature**: `009-zero-preseed-providers`  
**Date**: 2026-11-26

---

## 1. Entities & State Invariants

### Entity 1: `DistributedRateLimiter` Policy Store

Holds active sliding/token-bucket `RateLimitRule` lists keyed by canonical lowercase provider name (`std::unordered_map<std::string, std::vector<RateLimitRule>> rules_by_provider_`).

| State / Event | Contents of `rules_by_provider_` | `HasRules("groq")` / `HasRules("openrouter")` | Effect on `CheckAndConsume(provider)` |
|---|---|---|---|
| Default construction (`DistributedRateLimiter(pool)`) | `{}` (Empty) | `false` / `false` | Passes allowed (`allowed = true`, 0 Redis Lua calls) |
| Explicit opt-in (`LoadStandardPolicies()` or `(pool, true)`) | `{"groq": [...], "openrouter": [...]}` | `true` / `true` | Evaluates standard Groq (30 RPM / 1000 RPD) and OpenRouter (20 RPM / 50 RPD) rules |
| `ProviderRegistry::RegisterRateLimits(limiter)` / `SetRateLimiter()` | Only providers configured in `ProviderRegistry` with `rate_limits` | `true` iff explicitly configured in catalog | Enforces configured rules per registered provider; unconfigured providers pass |
| `ClearRules(provider)` / `ClearAllRules()` | Erases target provider or clears all entries (`{}`) | `false` | Clears cleanly with zero re-seeding |

---

### Entity 2: `TelemetryRegistry` Time-Series Store

Maintains thread-safe Prometheus series maps:
- `requests_counters_`: `std::map<std::string, std::shared_ptr<std::atomic<uint64_t>>>` (`model|provider|status`)
- `ratelimit_skips_counters_`: `std::map<std::string, std::shared_ptr<std::atomic<uint64_t>>>` (`provider`)
- `circuit_gauges_`: `std::map<std::string, std::shared_ptr<std::atomic<uint32_t>>>` (`provider`)
- `latency_histograms_`: `std::map<std::string, std::shared_ptr<Histogram>>` (`provider`)
- `exact_cache_hits_`, `semantic_cache_hits_`, `cache_misses_`: global cache atomics initialized to `0`

| Lifecycle / Call | Mutation to Provider Maps | Exported in `ToPrometheusFormat()` |
|---|---|---|
| Fresh construction (`TelemetryRegistry()`) | None (all 4 maps empty) | `# HELP` / `# TYPE` headers + cache counters (`0`), zero provider rows |
| `Reset()` | Clears all 4 maps; resets cache atomics to `0` | Zero provider rows; zero re-seeded `groq` or `openrouter` |
| `RegisterProvider(p)` (via `ProviderRegistry::RegisterProvider`) | Seeds `circuit_gauges_[p] = 0`, `latency_histograms_[p]`, `ratelimit_skips_counters_[p] = 0` | Emits initial `0` series for `p` only |
| `RegisterStandardProviders()` (explicit opt-in) | Calls `RegisterProvider("groq")` and `RegisterProvider("openrouter")` | Emits `groq` and `openrouter` initial series |
| Read queries (`GetCircuitBreakerState`, `GetUpstreamLatencySnapshot`, `GetRequestsTotal`, `GetRateLimitSkipsTotal`) | **No mutation** (`const` lookup returning `0` / empty snapshot if absent) | Unchanged (zero phantom rows created) |
| Active telemetry writes (`IncrementRequestsTotal`, `SetCircuitBreakerState`, `RecordUpstreamLatency`, `IncrementRateLimitSkipsTotal`) | Lazy-inserts entry for the observed `provider` if absent and updates count/gauge/histogram | Emits live series for actively observed provider |

---

### Entity 3: `ProviderRegistry` Configuration & Client Factory Propagation

- `RegisterProvider(config, &error_msg)`:
  - Validates `config`, normalizes `lower_name`, instantiates `IUpstreamClient` (via `client_factory_`) and `ICircuitBreaker` (via `breaker_factory_`), stores entry, pushes `config.rate_limits` into attached `rate_limiter_` (if non-null), and calls `TelemetryRegistry::Instance().RegisterProvider(lower_name)`.
- `LoadFromEnvironment(bool preseed_defaults = true, ClientFactory custom_client_factory = nullptr)`:
  - Scans configuration sources in priority order:
    1. Indexed `PROVIDER_N_NAME` (`N >= 1`)
    2. Comma-separated `TESSERA_PROVIDERS` (JSON catalogs handled via `LoadFromJson()`)
    3. Legacy credentials (`GROQ_API_KEY`, `OPENROUTER_API_KEY`)
    4. Offline mock fallback (`DefaultGroqConfig()`, `DefaultOpenRouterConfig()`) **only if** `preseed_defaults == true` and stages 1–3 produced zero providers.
  - When `custom_client_factory` is supplied, installs it on the constructed `ProviderRegistry` *before* registering discovered providers so `cli` tools (`gateway_client`) control live vs. mock clients without post-registration mutation.

---

### Entity 4: `gateway_client` Execution Stack State Transitions

```text
CLI args + Env vars
      │
      ├── Custom providers configured? ──► Loads ONLY configured providers (zero phantom groq/openrouter)
      │
      └── Zero explicit config? ─────────► LoadFromEnvironment(true, client_factory) loads offline mock defaults
                                                │
                                                ▼
                                     DistributedRateLimiter(pool)  [empty ctor]
                                     + provider_registry.RegisterRateLimits(*limiter)
                                                │
                                                ▼
                                     ModelRouter::CreateFromRegistry(provider_registry, limiter)
                                                │
                                                ▼
                                     CachedCompletionBackend(model_router, two_tier_cache)
                                                │
                                                ▼
                                     GatewayServer -> ProcessRequestSync(req, &resp)
```

---

## 2. Validation Rules Summary

1. **Empty Limiter Rule**: A default-constructed `DistributedRateLimiter` MUST return `HasRules("groq") == false`, `HasRules("openrouter") == false`, and empty `GetRules(...)` vectors.
2. **Empty Telemetry Rule**: Both newly constructed and freshly `Reset()` `TelemetryRegistry` instances MUST return `GetTrackedProviders().empty() == true` and emit zero `provider="..."` samples in `ToPrometheusFormat()`.
3. **Exclusive Custom Parity Rule**: When `ProviderRegistry` registers custom providers `{P_1, ..., P_K}`, `DistributedRateLimiter` and `TelemetryRegistry` MUST track only `{P_1, ..., P_K}` unless callers explicitly invoke `LoadStandardPolicies()`, `RegisterStandardProviders()`, or actively record traffic for another provider name.
4. **CLI Fidelity Rule**: `gateway_client` MUST bypass outer single-provider `RateLimitedBackend`, dispatch through `CachedCompletionBackend -> ModelRouter` with per-tier `CascadeRouter` enforcement, and honor `--health`, `--metrics`, `--failover-demo`, `--message`, `--model`, `--no-cache`, and optional provider keys without failing on offline unkeyed configurations.
