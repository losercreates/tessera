# Quickstart & Validation Guide: Zero-Preseed Provider Limiter & Telemetry Initialization

**Feature**: `009-zero-preseed-providers`  
**Date**: 2026-11-26  
**Status**: Verified

---

## 1. Prerequisites

Build all gateway targets and test binaries cleanly with Debug symbols and AddressSanitizer / UndefinedBehaviorSanitizer enabled:

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_SANITIZERS=ON \
  -DBUILD_TESTS=ON

cmake --build build --parallel $(nproc)
```

---

## 2. End-to-End Validation Scenarios

### Scenario 1: Zero-Preseed Limiter & Telemetry Initialization / Reset
Verifies that fresh `DistributedRateLimiter` and `TelemetryRegistry` instances (as well as freshly reset registries) hold zero pre-seeded `groq` or `openrouter` policies or metric series, while explicit opt-in helpers load legacy standard presets only when invoked.

```bash
./build/unit_tests --gtest_filter="RateLimiterTest.*:TelemetryTest.*"
```

**Expected Outcome**:
- Default-constructed `DistributedRateLimiter` reports zero rules for `groq` and `openrouter`.
- Newly initialized and freshly `Reset()` `TelemetryRegistry` returns zero tracked providers and omits `groq`/`openrouter` from `ToPrometheusFormat()`.
- Opt-in helpers (`LoadStandardPolicies()`, `RegisterStandardProviders()`) explicitly load standard preset limits/series in standalone tests cleanly.

---

### Scenario 2: Catalog-Exclusive Activation Across Custom Providers (CLI & Pipeline)
Verifies that registering custom providers (without `groq` or `openrouter`) activates quota rules and Prometheus series exclusively for those configured providers in both `ProviderRegistry` integration tests and `gateway_client`.

```bash
./build/unit_tests --gtest_filter="ProviderRegistryTest.*"
TESSERA_PROVIDERS=ollama,deepseek \
  PROVIDER_OLLAMA_ENDPOINT=http://localhost:11434/v1/chat/completions \
  PROVIDER_OLLAMA_MODEL=llama3.2 \
  PROVIDER_DEEPSEEK_ENDPOINT=https://api.deepseek.com/v1/chat/completions \
  PROVIDER_DEEPSEEK_MODEL=deepseek-chat \
  ./build/gateway_client --message "Custom provider smoke test" --metrics
```

**Expected Outcome**:
- `gateway_client` completes cleanly via `ProviderRegistry` + `ModelRouter` (defaulting unkeyed endpoints to offline simulated responses unless `--live` is supplied) without referencing `groq` or `openrouter`.
- Prometheus `--metrics` exposition lists `ollama` and `deepseek` and emits **zero** `groq` or `openrouter` series across `circuit_breaker_state`, `upstream_latency_ms`, and `gateway_ratelimit_skips_total`.

---

### Scenario 3: Offline `gateway_client` Failover Demo, Prometheus Gate & Full Sanitizer Regression Suite
Verifies backward compatibility of `gateway_client` diagnostics (`--health`, `--failover-demo`, `--metrics`), Prometheus CI validation script, and 100% CTest pass rate under ASan/UBSan.

```bash
./build/gateway_client --health
./build/gateway_client --failover-demo --message "Trigger tier failover" --metrics
bash scripts/verify_metrics_format.sh
ctest --test-dir build --output-on-failure
```

**Expected Outcome**:
- `--health` exits `0` reporting `SERVING`; `--failover-demo` demonstrates simulated Tier 0 failure transitioning cleanly through `CascadeRouter` to Tier 1 without dropped requests.
- `scripts/verify_metrics_format.sh` passes all Prometheus 0.0.4 assertions cleanly.
- All CTest targets (`unit_tests`, `integration_tests`, `ratelimit_concurrency_test`, `concurrency_test`) achieve 100% pass rate with zero ASan/UBSan violations or compiler warnings.
