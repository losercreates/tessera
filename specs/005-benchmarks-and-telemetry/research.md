# Research & Architectural Decisions: Synthetic Evaluation Harness, OpenTelemetry Metrics & Containerized Benchmarks

**Feature Branch**: `005-benchmarks-and-telemetry`  
**Date**: 2026-11-06  
**Status**: Completed  

---

## 1. OpenTelemetry Observability Architecture & In-Memory Metrics Registry

### Decision
Implement a zero-allocation/low-overhead, thread-safe OpenTelemetry-compatible metrics registry (`TelemetryRegistry`) capable of:
1. Recording atomic metrics across all gateway completion threads without mutex bottlenecks:
   - `gateway_requests_total{model="...", provider="...", status="..."}`: Monotonically increasing `uint64_t` atomic counter.
   - `gateway_cache_hits_total{tier="exact|semantic|miss"}`: Monotonically increasing `uint64_t` atomic counter.
   - `circuit_breaker_state{provider="..."}`: Atomic gauge (`0=CLOSED`, `1=OPEN`, `2=HALF-OPEN`).
   - `upstream_latency_ms{provider="..."}`: Lock-free exponential/linear bucketing histogram (buckets: `[1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000]`).
2. Exposing a Prometheus-compatible standard exposition text format (`# HELP`, `# TYPE`, metric lines with labels) over an embedded HTTP endpoint or programmatic string serializing method.
3. Measurement overhead budget: strictly $< 1\mu\text{s}$ per completed request.

### Rationale
- High-throughput gateways processing 5,000+ RPS cannot incur lock contention or heap allocations in telemetry hot paths. Using `std::atomic<uint64_t>` arrays indexed by label hashes or static enums guarantees $< 50\text{ns}$ update times.
- Prometheus exposition format is the universal cloud-native industry standard supported by OpenTelemetry Collector, Prometheus, Grafana, Datadog, and Kubernetes.
- Adhering to the OpenTelemetry semantic conventions for HTTP/gRPC gateways ensures seamless compatibility with upstream observability pipelines without third-party heavy binary dependencies.

### Alternatives Considered
- **Linking Full OpenTelemetry C++ SDK (`opentelemetry-cpp`) via vcpkg**: The official OpenTelemetry C++ SDK introduces heavy transitive dependencies (Abseil, gRPC, Curl, Protobuf, Thrift) that significantly inflate build times, binary size, and complexity. An in-tree, zero-dependency OpenTelemetry registry conforming to OpenTelemetry semantic conventions delivers the exact same metrics with zero build overhead and $< 1\mu\text{s}$ latency.
- **External Log Scraping (e.g. Vector/FluentBit parsing JSON logs)**: Parsing text logs to compute cache hit ratios and p95 latency introduces significant CPU overhead and high ingestion latency (seconds of lag). In-memory atomic counters provide sub-millisecond real-time accuracy.

---

## 2. Synthetic 1,000-Query Evaluation Dataset & Semantic Paraphrase Strategy

### Decision
Generate a deterministic, domain-diverse dataset of 1,000 test queries comprising:
1. **700 Unique Base Queries**: Spanning technical programming, system architecture, factual knowledge, code debugging, and mathematical reasoning.
2. **300 Paraphrased Duplicates (30%)**: Semantically identical reformulations of base queries featuring:
   - Lexical synonym replacement (e.g. "How do I reverse a linked list in C++?" $\rightarrow$ "Write C++ code to invert a singly linked list.")
   - Syntactic restructuring and question inversion.
   - Conversational prompt variations.
3. Workload Execution Flow:
   - Phase A (Cold Run): 100 queries to warm the caches and establish baseline cold upstream latency.
   - Phase B (Evaluation Run): Full 1,000 queries processed sequentially and under concurrent worker pools.
   - Measurement: Capture exact hit rate, semantic hit rate, false positive rate, upstream execution count, and calculate p50, p90, p95, and p99 latencies.
   - Quantitative SLA Verification:
     $$\text{Latency Reduction} = \frac{\text{Latency}_{\text{cold}} - \text{Latency}_{\text{warm}}}{\text{Latency}_{\text{cold}}} > 80\%$$
     $$\text{Token Savings} = \frac{\text{Tokens}_{\text{saved}}}{\text{Tokens}_{\text{total}}} > 25\%$$

### Rationale
- Constitution Principle III and VI require empirical verification that the Two-Tier cache architecture reduces p95 latency by up to 90% and slashes upstream token expenditure.
- A 30% paraphrase distribution simulates realistic enterprise microservice traffic where recurring questions appear with stylistic and syntactic variations.
- Generating the dataset as structured JSON or compiled C++ fixtures enables 100% offline reproducibility in CI pipelines without internet access or paid LLM API costs.

### Alternatives Considered
- **Random String Generation (Fuzzing)**: Random strings have near-zero semantic similarity, resulting in 0% Tier 2 cache hits and failing to validate vector search.
- **Querying External Public Benchmarks at Runtime (e.g. HuggingFace MTEB)**: Requires active internet access and external Python tools, violating hermetic build and offline CI constraints.

---

## 3. Containerized Orchestration Architecture (Docker Compose)

### Decision
Provide a containerized deployment configuration:
1. `Dockerfile`: Multi-stage build producing a lean runtime container:
   - Stage 1 (Builder): Ubuntu 24.04 with GCC 13, CMake 3.25+, Ninja, OpenSSL, compiling `tessera_core`, `gateway_server`, and benchmark binaries.
   - Stage 2 (Runtime): Minimal runtime image containing only required dynamic libraries, compiled binaries, and default ONNX embedding assets.
2. `docker-compose.yml`:
   - Service `redis-stack`: `redis/redis-stack-server:latest` with RediSearch and RedisJSON modules exposed on port 6379.
   - Service `tessera-gateway`: Gateway container exposing gRPC API (port 50051) and HTTP Metrics (port 9090).
   - Health check dependency: `tessera-gateway` waits for `redis-stack` to pass `redis-cli ping` before startup.
   - Environment variable injection via `.env` / environment block (`REDIS_HOST`, `REDIS_PORT`, `WORKER_THREADS`, `CACHE_TTL`).

### Rationale
- Enables single-command evaluation (`docker compose up -d`) across any host operating system (macOS, Linux, Windows WSL2).
- Ensures Redis Stack vector search modules and gateway binaries run in a tested, reproducible environment with identical kernel network parameters.

### Alternatives Considered
- **Kubernetes Helm Charts**: Overkill for single-node evaluation and local benchmarking; Docker Compose provides sufficient multi-container coordination with near-zero setup friction.
- **Single Monolithic Container (Redis + Gateway together)**: Violates container separation of concerns and prevents independent scaling of gateway replicas against a shared Redis Stack cluster.

---

## 4. Performance & Concurrency Profiling Strategy

### Decision
Develop a dedicated benchmark binary (`tools/evaluation_harness.cpp` and `tools/latency_benchmark.cpp`):
- Multi-threaded execution pool (configurable from 1 to 200 client threads).
- High-resolution timing using `std::chrono::steady_clock` to compute microsecond-level latency percentiles.
- Statistical report output in both human-readable console format and structured JSON for CI regression assertions.
- Resident Set Size (RSS) monitoring using `/proc/self/statm` to verify memory remains $< 500\text{MB}$ RAM throughout execution.

### Rationale
- Validates non-blocking concurrency, zero race conditions under sanitizers, and sustained throughput exceeding 5,000 RPS.

