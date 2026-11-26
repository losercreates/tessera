# Research & Technical Decisions: Zero-Preseed Provider Limiter & Telemetry Initialization

**Feature**: `009-zero-preseed-providers`  
**Date**: 2026-11-26  
**Scope**: Removing hardcoded Groq/OpenRouter pre-seeding from `DistributedRateLimiter` and `TelemetryRegistry`, enforcing `ProviderRegistry`-exclusive registration with opt-in test helpers, and migrating `gateway_client` to `ProviderRegistry` + `ModelRouter`.

---

## Decision 1: `DistributedRateLimiter` Zero-Preseed Constructor & Opt-In Helper

- **Decision**: Remove the unconditional `LoadStandardPolicies()` call from `DistributedRateLimiter::DistributedRateLimiter(std::shared_ptr<RedisConnectionPool> redis_pool)`, leaving `rules_by_provider_` completely empty on default construction. Keep `void LoadStandardPolicies()` as an explicit opt-in helper on `DistributedRateLimiter`, and optionally support a default-`false` constructor flag `explicit DistributedRateLimiter(std::shared_ptr<RedisConnectionPool> redis_pool, bool load_standard_policies = false)` so existing single-argument callers default cleanly to zero pre-seeding while explicit callers can opt in concisely.
- **Rationale**: Every production or test rate limiter constructed with a `ProviderRegistry` (`src/main.cpp`, `tools/gateway_client.cpp`, cascade tests) previously inherited hidden `groq` and `openrouter` token-bucket rules even when only custom providers (e.g., `ollama`, `deepseek`) were registered. Default-empty initialization ensures single-source-of-truth population from `ProviderRegistry::RegisterRateLimits(*rate_limiter)` or `ProviderRegistry::SetRateLimiter(rate_limiter)`, while preserving `LoadStandardPolicies()` avoids breaking standalone legacy tests cleanly.
- **Alternatives Considered**:
  - Deleting `LoadStandardPolicies()` outright: Rejected because multiple legacy unit and concurrency tests (`tests/unit/test_rate_limiter.cpp`, `tests/integration/test_ratelimit_concurrency.cpp`, `tests/integration/test_redis_resilience.cpp`) intentionally validate the standard Groq/OpenRouter RPM/RPD budgets without constructing a full `ProviderRegistry`. Keeping it as an explicit opt-in satisfies the feature spec directly.
  - Clearing rules inside `ProviderRegistry::RegisterRateLimits()`: Rejected as fragile; every direct user of `DistributedRateLimiter` outside `ProviderRegistry` would still carry phantom Groq/OpenRouter rules.

---

## Decision 2: `TelemetryRegistry` Zero-Preseed Initialization, Reset, and Explicit Opt-In

- **Decision**: Empty `TelemetryRegistry::TelemetryRegistry()` and `TelemetryRegistry::Reset()` so zero provider entries are pre-populated in `circuit_gauges_`, `latency_histograms_`, or `ratelimit_skips_counters_` at startup or after reset. Add `void RegisterStandardProviders();` on `TelemetryRegistry` as an explicit opt-in helper (registering `"groq"` and `"openrouter"` via `RegisterProvider()`). Preserve existing lazy series creation on active write paths (`IncrementRequestsTotal`, `SetCircuitBreakerState`, `RecordUpstreamLatency`, `IncrementRateLimitSkipsTotal`) and non-mutating zeroed defaults on read paths (`GetCircuitBreakerState`, `GetUpstreamLatencySnapshot`, `GetRequestsTotal`, `GetRateLimitSkipsTotal`).
- **Rationale**: Previously, `TelemetryRegistry` emitted phantom `circuit_breaker_state`, `upstream_latency_ms_*`, and `gateway_ratelimit_skips_total` series for `groq` and `openrouter` even when zero traffic or custom-only providers (`ollama`, `deepseek`) existed. Notice `Reset()` also re-seeded `groq` and `openrouter` asymmetrically (in gauges/histograms but not skips). Removing pre-seeding from both ctor and `Reset()` guarantees that exported Prometheus metrics strictly mirror providers registered through `ProviderRegistry::RegisterProvider()` (or actively observed during traffic). Read-only getters remain non-mutating so checking an unregistered provider never creates phantom Prometheus series.
- **Alternatives Considered**:
  - Requiring strict `RegisterProvider()` before `SetCircuitBreakerState` / `RecordUpstreamLatency`: Rejected because unit tests and standalone router tests (`CascadeRouterTest`, `EvaluationIntegrationTest`) record metrics for ad-hoc test provider IDs without instantiating `ProviderRegistry`. Preserving lazy creation on write paths guarantees full resilience without pre-seeding phantom defaults.

---

## Decision 3: `ProviderRegistry` Remains the Exclusive Registration Entry Point

- **Decision**: Retain `ProviderRegistry::RegisterProvider()` calling `TelemetryRegistry::Instance().RegisterProvider(lower_name)` and syncing `config.rate_limits` into an attached `rate_limiter_`, alongside `ProviderRegistry::RegisterRateLimits(DistributedRateLimiter&)` and `ProviderRegistry::SetRateLimiter(...)`. No changes to `LoadFromEnvironment(bool preseed_defaults)`: explicit env vars (`PROVIDER_N_NAME`, `TESSERA_PROVIDERS`, `GROQ_API_KEY`, `OPENROUTER_API_KEY`) load *only* configured providers (with `LoadFromJson` available for JSON catalogs); offline mock defaults (`DefaultGroqConfig`, `DefaultOpenRouterConfig`) apply only when `preseed_defaults = true` and zero explicit provider config exists; `LoadFromEnvironment(false)` loads nothing when unconfigured.
- **Rationale**: Auditing `src/routing/provider_registry.cpp` confirmed `ProviderRegistry` already propagates only registered providers cleanly, and its fallback `preseed_defaults` flag in `LoadFromEnvironment(true)` only triggers when zero explicit providers are specified. Previously, the leak came from `DistributedRateLimiter` and `TelemetryRegistry` unconditionally seeding `groq`/`openrouter` regardless of what `ProviderRegistry` loaded.

---

## Decision 4: `gateway_client` Pipeline Migration to `ProviderRegistry` + `ModelRouter` + `CachedCompletionBackend`

- **Decision**: Refactor `tools/gateway_client.cpp` cleanly to match `src/main.cpp`'s Feature 007/008 architecture:
  1. Parse CLI arguments (`--health`, `--metrics`, `--failover-demo`, `--live`, `--message`, `--model`, `--no-cache`, `--groq-key`, `--openrouter-key`, `--redis-host`, `--redis-port`, `--cache-ttl`, `--similarity`).
  2. If `--groq-key` or `--openrouter-key` was passed explicitly on CLI without custom env providers overriding them, apply those credentials cleanly before loading `ProviderRegistry::LoadFromEnvironment(true)` (skipping live-key shortcutting when `--failover-demo` needs offline multi-tier fallback defaults or logging clearly when `ProviderCount() < 2`).
  3. Supply a custom `ClientFactory` into `ProviderRegistry::LoadFromEnvironment(true, custom_client_factory)` so `gateway_client` creates `HttpUpstreamClient` when `(!cfg.api_key.empty() || live_mode) && !failover_demo`, and offline `MockUpstreamClient` otherwise (enqueuing simulated `429` on Tier 0 when `--failover-demo` is enabled).
  4. Attach empty `DistributedRateLimiter` fed solely via `provider_registry.RegisterRateLimits(*rate_limiter)`.
  5. Construct `model_router = ModelRouter::CreateFromRegistry(provider_registry, rate_limiter)` and wrap it cache-first with `pipeline = std::make_shared<CachedCompletionBackend>(model_router, two_tier_cache)`.
  6. Add `[[nodiscard]] std::shared_ptr<CascadeRouter> GetDefaultCascade() const;` to `ModelRouter` so `gateway_client` can query `CascadeRouterStats` (`successful_requests`, `failover_attempts`, `direct_bypasses`, `rate_limit_skips`) and iterate `provider_registry.GetRegisteredProviders()` to print per-provider `CircuitState` cleanly.
  7. Model selection & `--failover-demo`: If user did NOT specify `--model`, default `req.model` to: (a) the first registered provider's `default_model` in normal mode, or (b) `"auto"` when `--failover-demo` is active and `ProviderCount() >= 2` so `ModelRouter::ResolveCascade("auto")` resolves `default_cascade_` (`Tier 0 -> Tier 1`) instead of pinning a single-provider exact route; Tier 0 returns `429`, triggering transparent failover to Tier 1. If `ProviderCount() < 2` during `--failover-demo`, log a clear warning that no fallback tier is configured.
- **Rationale**: Using a factory-aware `LoadFromEnvironment` (or default param) guarantees `gateway_client` works seamlessly offline with zero API keys (preserving `scripts/verify_metrics_format.sh` CI runs), supports live HTTPS when keyed, supports `--failover-demo` across ANY configured 2+ providers, honors custom `TESSERA_PROVIDERS` cleanly without phantom `groq`/`openrouter` series, and inherits Feature 008's cache-first pipeline ordering.
- **Alternatives Considered**:
  - Constructing `FailoverRouter` from the first two `ProviderRegistry` entries: Rejected because it ignores 3+ tier cascades, drops model-based routing (`ModelRouter`), and leaves outer pre-cache rate limiting or 2-provider hardcoding in `gateway_client`.

---

## Decision 5: Test & Verification Gate Alignment

- **Decision**:
  - Update existing tests whose setup implicitly depended on default Groq/OpenRouter pre-seeding (`tests/unit/test_rate_limiter.cpp`, `tests/integration/test_ratelimit_concurrency.cpp`, `tests/integration/test_redis_resilience.cpp`) to call `rate_limiter->LoadStandardPolicies()` explicitly.
  - Keep `tests/unit/test_telemetry.cpp` verifying zero-preseed after init/reset (`GetTrackedProviders().empty()`, no `"groq"` / `"openrouter"` in `ToPrometheusFormat()`), non-mutating reads on unknown providers, lazy dynamic creation on active writes, AND explicit `RegisterStandardProviders()` opt-in.
  - Add new tests for `DistributedRateLimiter` empty init (`!HasRules("groq")`, `!HasRules("openrouter")`, unconstrained unconfigured pass-through, explicit `LoadStandardPolicies()`), custom-only `ProviderRegistry -> DistributedRateLimiter + TelemetryRegistry` parity (zero phantom vendors), and `gateway_client` verification under both zero-config offline defaults (`scripts/verify_metrics_format.sh`) and custom `TESSERA_PROVIDERS` smoke checks.
- **Rationale**: Keeps all legacy coverage intact while directly proving every zero-preseed guarantee and CLI regression gate under `-fsanitize=address,undefined`.
