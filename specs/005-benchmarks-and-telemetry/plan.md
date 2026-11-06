# Implementation Plan: Synthetic Evaluation Harness, OpenTelemetry Metrics & Containerized Benchmarks

**Branch**: `005-benchmarks-and-telemetry` | **Date**: 2026-11-06 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/005-benchmarks-and-telemetry/spec.md`

---

## Summary

Implement OpenTelemetry-compatible observability metrics (`gateway_requests_total`, `gateway_cache_hits_total`, `circuit_breaker_state`, `upstream_latency_ms`), Prometheus exposition text formatting, a synthetic 1,000-query evaluation harness containing 30% semantically equivalent paraphrased duplicates to validate $> 80\%$ p95 latency reduction and $> 25\%$ token cost savings under load, and a production-ready multi-stage `Dockerfile` and `docker-compose.yml` orchestrating the Tessera gateway with Redis Stack.

---

## Technical Context

**Language/Version**: C++20 (GCC 13.3+, Clang 18.1+ with `-std=c++20`)  
**Primary Dependencies**: Standard Library (`std::atomic`, `std::chrono`, `std::format`, `std::span`), OpenSSL (`libcrypto`, `libssl`), Threads, optional gRPC/Protobuf  
**Storage**: Redis Stack (RediSearch HNSW vector index + RedisJSON) in container, thread-safe in-memory metrics registry  
**Testing**: GoogleTest / compatibility harness, GMock, CTest under AddressSanitizer and UndefinedBehaviorSanitizer  
**Target Platform**: Linux x86_64 containerized (Docker Compose) and native server  
**Project Type**: High-performance distributed microservice gateway  
**Performance Goals**: $> 80\%$ p95 latency reduction on cached queries; $> 25\%$ token expenditure reduction; $\ge 5,000\text{ RPS}$ cached throughput under 200 concurrent threads; $< 1\mu\text{s}$ telemetry overhead per request  
**Constraints**: Resident Set Size strictly $< 500\text{MB}$ RAM; zero memory leaks or undefined behavior under ASan/UBSan; hermetic Docker multi-stage build  
**Scale/Scope**: 1,000 synthetic evaluation queries (700 unique base prompts + 300 paraphrased duplicates); multi-container deployment with Redis Stack  

---

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Requirement | Plan Compliance Status | Notes / Design Alignment |
|---|---|:---:|---|
| **I. Modern C++20 Architecture** | RAII, smart pointers, zero raw pointer ownership, async decoupling | **PASS** | Telemetry registry and evaluation runner use `std::shared_ptr`, `std::atomic`, strict RAII, and modular decoupling |
| **II. Determinism & Reliability** | Resilient routing, circuit breakers, rate limits, 99.99% availability | **PASS** | Telemetry tracks real-time circuit state (`circuit_breaker_state`) and verifies failover reliability during evaluations |
| **III. Two-Tier Caching** | Exact-match & ONNX semantic vector caching | **PASS** | Synthetic 1,000-query evaluation validates exact match hits and semantic vector search ($S \ge 0.95$) with 30% paraphrases |
| **IV. Quality Gate & Testing** | $\ge 90\%$ test coverage, ASan/UBSan clean, chaos validation | **PASS** | Unit tests for telemetry registry, integration tests for evaluation runner, and CTest matrix clean under ASan/UBSan |
| **V. CI/CD Reproducibility** | Hermetic toolchain, Docker Compose, OpenTelemetry metrics | **PASS** | Multi-stage `Dockerfile` and `docker-compose.yml` orchestrating Redis Stack and gateway with Prometheus metrics export |

---

## Project Structure

### Documentation (this feature)

```text
specs/005-benchmarks-and-telemetry/
├── spec.md                             # Feature specification
├── plan.md                             # Implementation plan
├── research.md                         # Architectural decisions & research findings
├── data-model.md                       # Domain models, telemetry structs & report schemas
├── contracts/
│   └── telemetry_and_eval_contract.md  # C++ interface contracts & Prometheus format spec
├── quickstart.md                       # Quickstart verification & run guide
└── checklists/
    └── requirements.md                 # Specification quality checklist
```

### Source Code Layout

```text
include/tessera/
├── telemetry/
│   ├── telemetry_registry.hpp          # TelemetryRegistry (counters, gauges, histograms)
│   └── metrics_exporter.hpp            # Prometheus exposition text serializer & HTTP handler
└── eval/
    ├── evaluation_dataset.hpp          # 1,000 synthetic test query dataset (700 base + 300 paraphrases)
    └── evaluation_runner.hpp           # EvaluationRunner executing workload and generating reports

src/
├── telemetry/
│   ├── telemetry_registry.cpp          # Atomic metric recording & Prometheus text formatting
│   └── metrics_exporter.cpp            # Embedded metrics endpoint serialization
└── eval/
    ├── evaluation_dataset.cpp          # Built-in synthetic test cases and paraphrase generation
    └── evaluation_runner.cpp           # Multi-threaded benchmark execution, p95 calculation, report output

tools/
└── evaluation_harness.cpp              # Standalone CLI evaluation tool (--queries, --threads, --json)

deploy/
├── Dockerfile                          # Multi-stage minimal runtime container build
└── docker-compose.yml                  # Redis Stack + Tessera Gateway orchestration

tests/
├── unit/
│   └── test_telemetry.cpp              # Metrics counters, gauges, histogram buckets, Prometheus format
└── integration/
    └── test_eval_integration.cpp       # 1,000-query benchmark run, latency reduction verification
```

**Structure Decision**: Telemetry components reside under `include/tessera/telemetry/` and `src/telemetry/`, evaluation harness components reside under `include/tessera/eval/` and `src/eval/`, and containerization assets reside in `deploy/` and root.

---

## Complexity Tracking

> **No Constitution violations detected.** Pure C++20 standard library implementation with zero external network dependencies required during build or test execution.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| None | N/A | N/A |
