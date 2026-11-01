# Phase 0 Research: gRPC Gateway Foundation

**Feature**: `001-grpc-gateway-foundation`  
**Date**: 2026-11-01  
**Status**: Completed  

---

## Technical Decisions & Trade-Offs

### 1. gRPC Server Concurrency Model

* **Decision**: Implement the asynchronous `grpc::ServerCompletionQueue` event-loop pattern using an explicit tag-based `CallData` state machine with a fixed worker thread pool (`std::thread::hardware_concurrency()`).
* **Rationale**:
  - Eliminates thread-per-request overhead, allowing the gateway to easily service 500+ concurrent requests on modest CPU allocations.
  - Matches Constitution Principle I (non-blocking async I/O).
  - Clean separation between incoming event dispatching and response generation.
* **Alternatives Considered**:
  - *Synchronous gRPC API*: Creates an OS thread per connection or blocks in a worker pool. Rejected because thread stack memory and context switching destroy p95 latency under high concurrency.
  - *gRPC Callback-based Reactor API*: Callback API is higher-level, but `ServerCompletionQueue` provides deterministic event loops, explicit backpressure control, and direct profiling capabilities.

---

### 2. Modern C++20 Standard & Memory Management

* **Decision**: Standardize on **C++20** with strict RAII ownership semantics.
* **Rationale**:
  - Mandated by Constitution Principle I.
  - Utilizes `std::unique_ptr` for exclusive lifetime management of `CallData` and service contexts.
  - Uses `std::span` for zero-copy string views and buffer inspection.
  - Leverages `std::format` for structured logging without stream overhead.
* **Alternatives Considered**:
  - *C++17*: Lacks standard `std::span` and `std::format`, requiring third-party dependencies (like `fmtlib` or `gsl::span`).
  - *Raw pointer event tags*: Rejected per Constitution Principle I (zero raw pointer ownership).

---

### 3. Protobuf Contract & Schema Design

* **Decision**: Define a unified `tessera.gateway.v1` namespace with `ChatCompletionRequest` and `ChatCompletionResponse` mirroring OpenAI-compatible structures while including gateway-specific tracing metadata (`correlation_id`, `cache_hit`, `tier`).
* **Rationale**:
  - Microservices already using standard OpenAI SDKs can map to this contract with zero friction.
  - Future milestones (caching, circuit breaking) can enrich metadata fields without breaking binary schema backward compatibility.
* **Alternatives Considered**:
  - *Generic JSON-over-gRPC*: Sacrifices binary serialization speed, type safety, and schema validation. Rejected.
  - *Custom proprietary RPC format*: Incompatible with industry standard LLM tooling. Rejected.

---

### 4. Milestone 1 Ingestion Mock Backend

* **Decision**: Abstract completion generation behind an `ICompletionBackend` C++ interface, with an in-memory `MockEchoBackend` for Milestone 1.
* **Rationale**:
  - Decouples the network ingestion layer from downstream caching and provider routers.
  - Allows isolating and stress-testing gRPC throughput, memory limits, and concurrency without external network dependencies.
  - Milestones 2 through 4 will provide `CachedCompletionBackend` and `RouterCompletionBackend` implementing this exact interface.
* **Alternatives Considered**:
  - *Hardcoding mock responses directly in gRPC service*: Tightly couples ingestion with mock logic, requiring refactoring in Milestone 2. Rejected.

---

### 5. Build System & Sanitizer Integration

* **Decision**: CMake 3.25+ with Ninja generator. Integrate GoogleTest via `FetchContent` and provide `-DENABLE_SANITIZERS=ON` to toggle `-fsanitize=address,undefined` in debug builds.
* **Rationale**:
  - Ensures hermetic local builds and CI reproducibility across GCC 13 and Clang 17 per Constitution Principle V.
  - Guarantees zero memory leaks and undefined behavior per Constitution Principle IV.

