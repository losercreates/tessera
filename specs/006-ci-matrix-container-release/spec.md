# Feature Specification: CI Matrix & Automated Container Release Pipeline

**Feature Branch**: `006-ci-matrix-container-release`  
**Created**: 2026-11-14  
**Status**: Draft  
**Input**: User description: "CI Matrix & Automated Container Release Pipeline: Extend GitHub Actions CI workflow to execute the 1,000-query evaluation harness under AddressSanitizer, validate Prometheus metrics export, automate multi-stage Docker image builds with health checks, and enforce zero-leak verification across GCC 13 and Clang 17."

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Automated High-Scale Synthetic Evaluation in CI (Priority: P1) 🎯 MVP

As a system maintainer and release engineer, I want the continuous integration pipeline to automatically execute the synthetic 1,000-query evaluation harness on every pull request and push to the primary branch, so that latency regressions, cache hit ratio drops, token cost anomalies, and memory bloat are detected before code is merged.

**Why this priority**: Continuous validation of the Two-Tier cache SLA (> 80% p95 latency reduction, > 25% token cost savings) is essential to prevent performance degradation over time. Automated gating ensures that regressions cannot silently enter production.

**Independent Test**:
1. Submit a commit or trigger the workflow.
2. Verify that the 1,000-query evaluation suite runs to completion in CI.
3. Verify that measured latency percentiles (p50, p90, p95, p99), token cost savings, and resident memory RSS (< 500MB) satisfy all performance gates.

**Acceptance Scenarios**:
1. **Given** code changes submitted in a pull request,  
   **When** the CI workflow triggers,  
   **Then** the synthetic evaluation harness executes 1,000 queries (with 30% paraphrases), confirming $\ge 80\%$ p95 latency reduction and $\ge 25\%$ token savings with 100% success rate.
2. **Given** an intentional regression in cache retrieval or an unexpected memory leak,  
   **When** the evaluation harness executes in CI,  
   **Then** the workflow immediately fails with an informative error log identifying the unmet performance threshold.

---

### User Story 2 - Dual-Compiler Matrix & Zero-Tolerance Memory Sanitizers (Priority: P1)

As a systems engineer, I want automated CI validation across both GCC 13 and Clang 17 with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) actively enabled, so that memory corruptions, buffer overflows, use-after-free conditions, memory leaks, and undefined behaviors are caught across multiple compiler backends.

**Why this priority**: Non-negotiable per Constitution Principle IV and Principle V. Multi-compiler matrix builds guarantee portability, while sanitizer-clean execution ensures production stability under extreme concurrency.

**Independent Test**:
1. Trigger a matrix run of the CI workflow.
2. Confirm both GCC 13 (Release) and Clang 17 (Debug + ASan/UBSan) execute all unit tests, integration tests, concurrency stress tests, and the evaluation harness.
3. Confirm 0 errors, 0 warnings, and 0 memory leaks across both compiler jobs.

**Acceptance Scenarios**:
1. **Given** any pull request or push,  
   **When** the CI matrix runs,  
   **Then** both GCC 13 and Clang 17 configurations compile cleanly with `-Wall -Wextra -Wpedantic` and pass all test suites.
2. **Given** code containing an unaligned pointer dereference or dangling memory allocation,  
   **When** Clang with AddressSanitizer executes the test suite,  
   **Then** the test runner immediately aborts, surfacing the exact stack trace and failing the CI job.

---

### User Story 3 - Automated Container Build & Live Health Probe Smoke Testing (Priority: P2)

As a DevOps engineer, I want CI to build the multi-stage production container image, launch the multi-container stack with Redis Stack and the gateway, and run live health check probes, so that container packaging, non-root user permissions, and deployment runtime configurations are continuously verified.

**Why this priority**: A passing unit test suite does not guarantee that container packaging, entrypoint scripts, dynamic port configurations, or network links with Redis Stack will succeed in production environments.

**Independent Test**:
1. Run the container validation job in CI.
2. Build the multi-stage `Dockerfile`.
3. Launch the container stack with Docker Compose and verify that health checks report healthy within 30 seconds.
4. Issue completion queries against the containerized gateway.

**Acceptance Scenarios**:
1. **Given** the repository source tree,  
   **When** the container build job runs,  
   **Then** the multi-stage Docker build produces a hardened runtime image with an unprivileged service user (`tessera`, UID 10001).
2. **Given** the running container stack in CI,  
   **When** container health check probes execute,  
   **Then** both `tessera-redis-stack` and `tessera-gateway` transition to `healthy` state within 30 seconds.

---

### User Story 4 - Metrics Exposition Verification & Build Artifact Archival (Priority: P2)

As an observability engineer and auditor, I want CI to validate Prometheus metrics exposition formatting and archive benchmark reports as workflow artifacts, so that performance history and telemetry compliance can be tracked and compared across releases.

**Why this priority**: Observability contracts must remain backward-compatible with monitoring scrapers (Prometheus / OpenTelemetry collector). Archiving evaluation reports enables long-term trend analysis.

**Independent Test**:
1. Run the metrics validation step in CI.
2. Verify that `gateway_requests_total`, `gateway_cache_hits_total`, `circuit_breaker_state`, and `upstream_latency_ms` are exposed conforming to Prometheus 0.0.4 text format.
3. Download the archived JSON report artifact from the completed workflow run.

**Acceptance Scenarios**:
1. **Given** the gateway client executing with `--metrics`,  
   **When** CI inspects the output,  
   **Then** all required metric names, types (`counter`, `gauge`, `histogram`), and labels match the Prometheus exposition contract.
2. **Given** a completed evaluation harness execution,  
   **When** the CI workflow concludes,  
   **Then** `eval_report.json` and Markdown summaries are uploaded as GitHub Actions artifacts retained for audit.

---

### Edge Cases

- **CI Runner Resource Contention**: GitHub Actions hosted runners have limited vCPUs (2 cores) and memory (7GB); evaluation harness concurrency must be tuned (e.g., 4-8 threads) to prevent CPU starvation or false timeout failures.
- **Hermetic Execution (No External API Keys)**: CI must run completely offline without external network dependencies or real provider API keys, utilizing the deterministic mock upstream engine.
- **Docker Layer Caching**: Container builds in CI should use GitHub Actions caching to minimize build times while ensuring clean layer separation.
- **Sanitizer Overhead in Containers**: Sanitizer instrumentation increases binary size and RAM usage; release container builds must use optimized release settings without sanitizer overhead, while CI test runners retain ASan/UBSan.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: CI workflow MUST execute the synthetic evaluation harness with 1,000 queries (30% paraphrased duplicates) on all pull requests and pushes to `master` / `main`.
- **FR-002**: CI workflow MUST enforce SLA quality gates on the evaluation harness: $\ge 80\%$ p95 latency reduction on cached queries, $\ge 25\%$ token cost savings, and $< 500\text{MB}$ peak resident memory.
- **FR-003**: CI workflow MUST execute a dual-compiler matrix containing GCC 13 (Release mode) and Clang 17 (Debug mode).
- **FR-004**: CI workflow MUST enable AddressSanitizer and UndefinedBehaviorSanitizer during the Clang matrix job, failing on any detected memory leaks, buffer overflows, or undefined behavior.
- **FR-005**: CI workflow MUST run all unit tests, integration tests, concurrency stress tests (500 clients), and rate limit tests (200 threads) across the matrix.
- **FR-006**: CI workflow MUST build the multi-stage production container image from `Dockerfile` / `deploy/Dockerfile`.
- **FR-007**: CI workflow MUST launch the multi-container stack via `docker compose` and verify that Redis Stack and the gateway pass automated health check probes.
- **FR-008**: CI workflow MUST validate Prometheus metrics exposition format output by `gateway_client --metrics`.
- **FR-009**: CI workflow MUST archive the evaluation benchmark report (`eval_report.json` and Markdown summary) as downloadable workflow artifacts.
- **FR-010**: All CI jobs MUST operate completely hermetically without requiring external live network access or provider credentials.

---

### Key Entities

- **CIWorkflow**: Automated GitHub Actions pipeline orchestrating linting, compilation matrix, test suites, evaluation benchmarks, container builds, and artifact archival.
- **CompilerMatrixJob**: Matrix execution configuration defining compiler binary (`gcc`/`g++` vs `clang`/`clang++`), build type (`Release` vs `Debug`), and sanitizer flags (`-fsanitize=address,undefined`).
- **EvaluationBenchmarkArtifact**: Persisted machine-readable JSON metrics artifact capturing total queries, hit ratios, p50/p90/p95/p99 latencies, throughput, and memory RSS for historical tracking.
- **ContainerReleasePackage**: Multi-stage hardened OCI container image packaging the compiled gateway server and client tools.

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of pull requests and commits to primary branches execute the full 1,000-query evaluation harness with 0 false positive failures.
- **SC-002**: Dual-compiler matrix (GCC 13 and Clang 17 with ASan/UBSan) completes full compilation, test execution, and benchmark evaluation within 10 minutes on standard CI runners.
- **SC-003**: Zero memory leaks or undefined behavior detected across the entire test matrix under AddressSanitizer and UndefinedBehaviorSanitizer.
- **SC-004**: Containerized stack (`docker compose up`) builds and enters healthy serving state in $< 60$ seconds in CI.
- **SC-005**: Evaluation report JSON artifacts are archived and available for download on 100% of successful CI runs.

---

## Assumptions

- Hosted GitHub Actions Ubuntu runners (`ubuntu-latest` / `ubuntu-24.04`) provide Docker and Docker Compose preinstalled.
- Redis Stack container image (`redis/redis-stack-server:latest`) is accessible from Docker Hub during container integration tests.
- Offline evaluation runs using `MockUpstreamClient` with simulated latency (25ms) provide deterministic, reproducible benchmark numbers suitable for CI gating.
- Git repository uses `master` or `main` as the default target branch.

