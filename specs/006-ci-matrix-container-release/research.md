# Technical Research: CI Matrix & Automated Container Release Pipeline

**Feature**: `006-ci-matrix-container-release`  
**Date**: 2026-11-14  
**Status**: Completed  

---

## 1. Dual-Compiler Matrix & Sanitizer Optimization

### Context
Constitution Principle IV and Principle V mandate dual-compiler verification across GCC 13 and Clang 17, with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) active on Clang builds.

### Decision
Structure the primary `build-and-test` job as a GitHub Actions matrix:
- **Matrix Slice 1**: GCC 13, `Release` mode (`-O3`), sanitizers disabled. Focus: Maximum optimization verification, release compilation warnings, pure throughput benchmarking.
- **Matrix Slice 2**: Clang 17, `Debug` mode (`-g -O1`), AddressSanitizer and UndefinedBehaviorSanitizer enabled (`-fsanitize=address,undefined -fno-omit-frame-pointer`). Focus: Zero memory leaks, boundary checks, unaligned accesses, pointer lifetime validation.

### Rationale
- Running sanitizers on Clang in Debug/O1 provides fast compilation, accurate symbol traces, and strict checking without exceeding CI job runtime budgets.
- Running GCC in Release mode verifies standard `-Wall -Wextra -Wpedantic` compilation without sanitizer overhead.

### Alternatives Considered
- *ThreadSanitizer (TSan) in the same job*: TSan is mutually exclusive with ASan in Clang/GCC (cannot pass `-fsanitize=address,thread`). ASan and UBSan provide the primary leak, use-after-free, and bounds protections required for memory safety.

---

## 2. 1,000-Query Evaluation Harness Execution in CI

### Context
FR-001 and FR-002 require automated execution of the 1,000-query evaluation harness (`./build/evaluation_harness`) within CI. GitHub Actions hosted runners (`ubuntu-latest`) have 2 vCPUs and 7GB RAM.

### Decision
- Execute `./build/evaluation_harness --queries=1000 --threads=4 --output-json=eval_report.json`.
- Concurrency is configured to 4 threads (instead of 8 or 16) to prevent thread context-switching thrash on 2-core runners while retaining multi-threaded cache ingestion.
- The evaluation harness operates hermetically: `MockUpstreamClient` simulates a realistic 25ms upstream latency, producing deterministic baseline vs cached comparison metrics without external network dependence or API cost.
- Add an automated assertion step parsing `eval_report.json` with Python or `jq` to verify:
  - `failed_queries == 0`
  - `p95_latency_reduction_pct >= 80.0`
  - `token_savings_pct >= 25.0`
  - `max_rss_mb < 500.0`

### Alternatives Considered
- *Running 100 queries instead of 1,000*: Rejected because Constitution Principle III and Milestone 6 explicitly mandate 1,000 queries with 30% paraphrases to prove statistical significance. 1,000 queries take $< 0.5$ seconds with 4 threads.

---

## 3. Container Build & Smoke Testing Architecture

### Context
FR-006 and FR-007 require CI to build the multi-stage production container image and verify health check readiness within a multi-container environment.

### Decision
Create a separate GitHub Actions job `container-smoke-test` (or staged step):
1. Build container image: `docker build -t tessera-gateway:ci .`
2. Launch multi-container stack via `docker compose up -d --wait` or loop polling container health status (`docker compose ps`).
3. Verify that Redis Stack is healthy with RediSearch and RedisJSON modules loaded.
4. Execute health probe inside the gateway container: `docker exec tessera-gateway /usr/local/bin/gateway_client --health`.
5. Issue completion request: `docker exec tessera-gateway /usr/local/bin/gateway_client --message "Docker smoke test" --metrics`.
6. Gracefully tear down with `docker compose down -v`.

### Rationale
- Running container verification as a dedicated job allows build matrix caching and keeps compile-test-sanitize loops fast.
- Validates that non-root user permissions (`tessera:tessera`, UID 10001), shared libraries (`libssl3`), and port bindings (`50051`, `9090`) behave identically in containerized environments as they do on bare metal.

---

## 4. Telemetry Validation & Artifact Archival

### Context
FR-008 and FR-009 require validating Prometheus text exposition formatting and archiving evaluation benchmark reports.

### Decision
- **Metrics Validation**: Run `./build/gateway_client --metrics` and assert presence of expected metric descriptors (`gateway_requests_total`, `gateway_cache_hits_total`, `circuit_breaker_state`, `upstream_latency_ms`).
- **Artifact Upload**: Use `actions/upload-artifact@v4` with `name: evaluation-report-${{ matrix.compiler }}` to archive:
  - `eval_report.json`
  - Markdown benchmark summary

### Rationale
- Archiving JSON artifacts creates an immutable audit trail of latency and throughput percentiles across commits, supporting performance regression tracking across releases.

