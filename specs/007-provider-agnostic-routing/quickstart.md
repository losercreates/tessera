# Quickstart & Validation Guide: Provider-Agnostic Routing & Dynamic Model Cascade

**Feature**: `007-provider-agnostic-routing`  
**Date**: 2026-11-19  
**Status**: Completed  

---

## 1. Prerequisites

Ensure dependencies and tools are built and available:
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
```

---

## 2. Validation Scenarios

### Scenario 1: Verify Registration of Multiple Providers (Local & Cloud)
Demonstrates configuring an arbitrary custom provider alongside standard providers without recompilation.

```bash
# 1. Start gateway with multi-provider environment configuration
PROVIDER_OLLAMA_ENDPOINT="http://localhost:11434/v1/chat/completions" \
PROVIDER_OLLAMA_MODEL="llama3.2" \
./build/gateway_server --port=50051 --threads=4 --mock-redis &
SERVER_PID=$!
sleep 1

# 2. Verify server banner displays all registered providers in its catalog
# Expected output contains:
# Upstream Providers: [ollama, groq, openrouter]

# 3. Stop server
kill -TERM $SERVER_PID
```

---

### Scenario 2: Model-Based Dynamic Routing
Verify that dispatching requests with different model identifiers directs traffic to the appropriate provider.

```bash
# Dispatch request targeting deepseek model
./build/gateway_client --message "Write a quicksort in C++" --model="deepseek-chat"

# Dispatch request targeting local ollama model
./build/gateway_client --message "Explain recursion" --model="llama3.2"

# Dispatch request targeting standard groq model
./build/gateway_client --message "Tell me a joke" --model="llama-3.1-8b-instant"
```
**Expected Outcome**: Each request is routed to its mapped provider target, and response reflects successful completion.

---

### Scenario 3: $N$-Tier Priority Fallback Cascade Under Chaos Faults
Simulate an outage on primary and secondary providers to verify transparent recovery on tertiary tier.

```bash
# Execute chaos cascade test injecting outages on Tiers 1 and 2
./build/unit_tests --gtest_filter="CascadeRouterTest.TripsAcrossNTiersWithZeroLoss"
```
**Expected Outcome**: Request transparently cascades across Tier 1 (trips breaker) $\rightarrow$ Tier 2 (trips breaker) $\rightarrow$ Tier 3 (succeeds) with 100% completion rate and zero dropped calls.

---

### Scenario 4: Isolated Per-Provider Rate Limiting
Validate that quota saturation on one provider does not throttle or affect another provider.

```bash
# Execute integration test for multi-provider rate and fault isolation
./build/integration_tests --gtest_filter="ProviderRateIsolationIntegrationTest.*"
```
**Expected Outcome**: Provider A reaches capacity and returns rate limit responses; Provider B concurrently processes requests at full throughput.

---

### Scenario 5: Dynamic Telemetry Tagging & Metrics Verification
Verify that Prometheus metrics dynamically track every registered provider.

```bash
./build/gateway_client --metrics > metrics_out.txt

# Inspect metrics for multi-provider labels
grep "gateway_requests_total{.*provider=" metrics_out.txt
grep "circuit_breaker_state{provider=" metrics_out.txt
grep "upstream_latency_ms_count{provider=" metrics_out.txt
```
**Expected Outcome**: Telemetry metrics report distinct counters, gauges, and histograms for each configured provider.
