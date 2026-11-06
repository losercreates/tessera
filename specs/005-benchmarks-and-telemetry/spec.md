# Feature Specification: Synthetic Evaluation Harness, OpenTelemetry Metrics & Containerized Benchmarks

**Feature Branch**: `005-benchmarks-and-telemetry`  
**Created**: 2026-11-06  
**Status**: Draft  
**Input**: User description: "Synthetic Evaluation Harness, OpenTelemetry Metrics & Containerized Benchmarks: Implement OpenTelemetry observability metrics (gateway_requests_total, gateway_cache_hits_total by tier, circuit_breaker_state, upstream_latency_ms), a synthetic evaluation harness running 1,000 queries with 30% paraphrased duplicates to validate > 80% p95 latency reduction and token cost savings under load, and Docker Compose environment orchestrating the Tessera gateway with Redis Stack."

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Real-Time Observability & Metrics Export (Priority: P1) 🎯 MVP

As an SRE and gateway operations engineer, I want standardized real-time observability metrics tracking aggregate requests, cache hit ratios per tier, circuit breaker states, and upstream latencies, so that I can monitor service health, detect upstream anomalies, and track cache efficiency in production dashboards.

**Why this priority**: Production gateways must be fully observable. Operators require visibility into exact-match and semantic cache hit distributions, circuit trip events, and upstream response times to guarantee 99.99% availability and prevent silent failures.

**Independent Test**:
1. Start the gateway service and issue a sequence of completion requests with varied prompt content and cache flags.
2. Query the metrics collection endpoint or registry.
3. Verify that `gateway_requests_total` increments per status and model, `gateway_cache_hits_total` tracks hits across Tier 1 (exact) and Tier 2 (semantic), `circuit_breaker_state` exposes real-time provider availability, and latency histograms record elapsed durations.

**Acceptance Scenarios**:
1. **Given** an operational gateway receiving client traffic,  
   **When** chat completion requests are completed,  
   **Then** `gateway_requests_total` counter increments tagged with model and outcome status.
2. **Given** requests that match cached entries in Tier 1 or Tier 2,  
   **When** cache hits occur,  
   **Then** `gateway_cache_hits_total` increments with label `tier="exact"` or `tier="semantic"` accordingly.
3. **Given** an upstream provider whose circuit breaker trips to `OPEN`,  
   **When** metrics are polled,  
   **Then** `circuit_breaker_state` reflects the tripped state (`1` for `OPEN`, `0` for `CLOSED`, `2` for `HALF-OPEN`) for that provider tag.
4. **Given** upstream requests dispatched to external or mock providers,  
   **When** responses return,  
   **Then** `upstream_latency_ms` records the duration in distribution buckets.

---

### User Story 2 - Synthetic 1,000-Query Evaluation Harness (Priority: P1)

As an ML/performance engineer, I want an automated evaluation workload runner that executes 1,000 representative completion queries containing 30% paraphrased duplicates against the gateway pipeline, so that I can objectively measure and verify cache hit rates, p95 latency reductions, and upstream token cost savings under realistic traffic conditions.

**Why this priority**: Caching efficiency must be validated scientifically. A reproducible workload with semantic paraphrases proves that embedding inference and vector similarity search yield real-world latency reductions (> 80% p95 drop) and token expenditure savings (> 25%).

**Independent Test**:
1. Run the evaluation harness against the gateway with a test dataset of 1,000 queries (700 unique base queries + 300 semantically paraphrased duplicates).
2. Measure and assert that cold queries populate both cache tiers.
3. Assert that paraphrased duplicates trigger Tier 2 semantic cache hits (similarity $\ge 0.95$).
4. Generate a consolidated evaluation report detailing cache hit percentage, p50/p95/p99 latency comparisons (cold vs warm), and estimated token cost savings.

**Acceptance Scenarios**:
1. **Given** an evaluation dataset with 1,000 queries containing 30% semantically equivalent duplicates,  
   **When** the workload harness executes sequentially or under controlled concurrency,  
   **Then** all 1,000 requests complete with 100% success rate.
2. **Given** cached responses in the vector index,  
   **When** semantically paraphrased queries arrive,  
   **Then** the harness records Tier 2 semantic cache hits with cosine similarity $\ge 0.95$.
3. **Given** completed evaluation execution,  
   **When** the metrics report is calculated,  
   **Then** p95 latency for warm queries demonstrates $> 80\%$ reduction compared to cold upstream calls, and total upstream token usage drops by $> 25\%$.

---

### User Story 3 - Reproducible Containerized Orchestration (Docker Compose) (Priority: P2)

As a DevOps engineer and evaluator, I want a complete, self-contained multi-container deployment defined in Docker Compose orchestrating the gateway, Redis Stack (with RediSearch and RedisJSON), and diagnostic tools, so that the entire system can be spun up with a single command for local evaluation, CI testing, and production deployment.

**Why this priority**: Developers and automated CI systems need hermetic, push-button environments. Container orchestration ensures Redis Stack vector search modules, ONNX embedding models, and gateway binaries interoperate seamlessly across any host OS without manual dependency setup.

**Independent Test**:
1. Execute `docker compose up -d`.
2. Verify Redis Stack initializes with RediSearch and RedisJSON modules loaded.
3. Verify the gateway container starts up, connects to Redis Stack, passes health check probes, and serves traffic on configured gRPC and HTTP metrics ports.
4. Issue test completions and verify full end-to-end operation inside containers.

**Acceptance Scenarios**:
1. **Given** a Docker-enabled host,  
   **When** `docker compose up -d` is executed,  
   **Then** both Redis Stack and the Tessera gateway containers start up and become healthy within 30 seconds.
2. **Given** the containerized environment is running,  
   **When** client requests target the gateway's exposed container port,  
   **Then** requests are served cleanly with exact and semantic caching active in Redis Stack.
3. **Given** environment variables for API keys and configuration,  
   **When** containers initialize,  
   **Then** the gateway reads configuration dynamically from container environment files without hardcoded secrets.

---

### User Story 4 - Automated Throughput & Latency Profiling Tooling (Priority: P2)

As a performance test engineer, I want standalone benchmarking CLI tools that measure requests per second (RPS) and latency percentiles under varying concurrency levels (10 to 200 concurrent clients), so that performance regressions can be caught before release.

**Why this priority**: Gateways must sustain high concurrency without thread contention or memory growth. Dedicated profiling tools provide continuous automated verification of throughput and memory stability under sustained load.

**Independent Test**:
1. Launch the benchmarking tool configured for 200 concurrent threads and 10,000 requests.
2. Measure aggregate RPS throughput, p50, p95, and p99 latency percentiles.
3. Verify memory usage of the gateway process remains bounded ($< 500\text{MB}$ RAM) throughout the run.

**Acceptance Scenarios**:
1. **Given** sustained high-concurrency traffic (200 threads),  
   **When** the benchmark executes,  
   **Then** throughput exceeds 5,000 requests per second for cached completions with 0% error rate.
2. **Given** long-running benchmark execution,  
   **When** process resident memory is measured,  
   **Then** memory remains stable below 500MB without memory leaks or unbounded buffer growth.

---

### Edge Cases

- **Cold Start Latency**: First-time evaluation queries have zero cache population and must execute against upstream providers or mocks without timeouts or false cache hits.
- **Paraphrase Similarity Boundaries**: Paraphrased queries with cosine similarity between 0.90 and 0.949 must correctly register as cache misses (not false positive hits) per the strict 0.95 threshold.
- **Metrics Scraping Under Extreme Load**: Polling metrics during peak load (thousands of RPS) must not block or slow down request processing threads.
- **Container Network Partition**: If Redis Stack becomes temporarily unreachable inside the container network, the gateway must fail open gracefully, serving upstream requests directly without container crash loops.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST collect and record `gateway_requests_total` counter metric labeled by model, provider, and response status code.
- **FR-002**: System MUST collect and record `gateway_cache_hits_total` counter metric partitioned by cache tier (`tier="exact"`, `tier="semantic"`, and `tier="miss"`).
- **FR-003**: System MUST collect and expose `circuit_breaker_state` gauge metric for each configured upstream provider (`0=CLOSED`, `1=OPEN`, `2=HALF-OPEN`).
- **FR-004**: System MUST collect and record `upstream_latency_ms` histogram metrics tracking distribution of upstream provider response times.
- **FR-005**: System MUST provide an OpenTelemetry-compatible metrics export mechanism and Prometheus-compatible text representation endpoint.
- **FR-006**: System MUST provide a synthetic evaluation harness tool capable of executing a configurable workload of queries (default: 1,000 queries).
- **FR-007**: The synthetic evaluation dataset MUST contain 30% semantically equivalent paraphrased queries designed to trigger Tier 2 semantic cache retrieval.
- **FR-008**: The evaluation harness MUST compute and report comprehensive benchmark metrics: total queries, cache hit percentage (exact vs semantic), p50, p95, p99 latencies, and token cost reduction.
- **FR-009**: The evaluation harness MUST validate that warm semantic cache hits achieve $\ge 80\%$ reduction in p95 latency compared to cold upstream response times.
- **FR-010**: System MUST provide a production-ready `Dockerfile` multi-stage build producing a minimal runtime container image.
- **FR-011**: System MUST provide a `docker-compose.yml` specification orchestrating the gateway, Redis Stack with vector modules, and health check validation.
- **FR-012**: The container environment MUST support dynamic configuration via environment variables (`REDIS_HOST`, `REDIS_PORT`, `GATEWAY_PORT`, `WORKER_THREADS`, `CACHE_TTL`, `SIMILARITY_THRESHOLD`).
- **FR-013**: System MUST provide automated health check probes in containers verifying both network responsiveness and cache backend connectivity.
- **FR-014**: System MUST maintain process memory usage below 500MB RAM throughout sustained evaluation workloads.
- **FR-015**: Benchmark tools MUST generate reproducible markdown and JSON summary reports detailing latency distributions and throughput.

---

### Key Entities

- **MetricRecord**: Data structure capturing individual telemetry observations: metric name, value, timestamp, and dimensional key-value labels.
- **TelemetryCollector**: Centralized thread-safe registry collecting counters, gauges, and histograms, compatible with OpenTelemetry semantic conventions.
- **EvaluationDataset**: Structured collection of 1,000 test cases with base prompts, ground-truth expected answers, paraphrased variants, and category tags.
- **EvaluationResult**: Aggregated benchmark output containing total requests, hit rates by tier, latency percentiles (p50, p90, p95, p99), token usage, and cost reduction ratios.
- **ContainerProfile**: Orchestration specification encapsulating service dependencies, port mappings, resource constraints, and health probes.

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Synthetic evaluation of 1,000 queries confirms $> 80\%$ reduction in p95 latency for semantically cached queries compared to cold upstream calls.
- **SC-002**: Upstream token consumption drops by $> 25\%$ across the 1,000-query evaluation run due to Tier 1 and Tier 2 cache interception.
- **SC-003**: 100% of Prometheus-compatible metrics (`gateway_requests_total`, `gateway_cache_hits_total`, `circuit_breaker_state`, `upstream_latency_ms`) are accurately captured with $< 1\mu\text{s}$ measurement overhead per request.
- **SC-004**: Entire containerized stack (`docker compose up`) builds and enters healthy serving state in $< 60$ seconds on standard environments.
- **SC-005**: Sustained high-throughput benchmarking demonstrates $\ge 5,000$ requests per second for cached completions with 0% dropped requests.
- **SC-006**: Resident memory of the gateway remains strictly $< 500\text{MB}$ RAM during execution of the full 1,000-query evaluation suite.

---

## Assumptions

- Redis Stack 7.2+ container image (`redis/redis-stack-server:latest`) is used to provide native RediSearch and RedisJSON capabilities.
- The evaluation dataset can run offline using `MockUpstreamClient` with realistic simulated upstream latency (e.g. 150ms-300ms) to allow CI pipelines to verify latency reductions deterministically without live API expenditure.
- When live credentials (`GROQ_API_KEY`, `OPENROUTER_API_KEY`) are provided in `.env`, the harness can optionally run against live external endpoints.
- Metrics endpoint is accessible via standard HTTP GET `/metrics` or gRPC diagnostics port.

