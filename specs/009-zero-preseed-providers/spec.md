# Feature Specification: Zero-Preseed Provider Limiter & Telemetry Initialization

**Feature Branch**: `009-zero-preseed-providers`  
**Created**: 2026-11-26  
**Status**: Draft  
**Input**: User description: "Zero-Preseed Provider Limiter & Telemetry Initialization: Remove unconditional hardcoded pre-seeding of Groq and OpenRouter policies, circuit state gauges, and latency histograms from DistributedRateLimiter and TelemetryRegistry initialization and reset paths. Register rate-limit rules and telemetry series exclusively for providers configured via ProviderRegistry (preserving an explicit opt-in helper for standalone unit tests). Update gateway_client to construct its pipeline from ProviderRegistry and ModelRouter rather than legacy Groq/OpenRouter FailoverRouter defaults, while keeping all existing unit, integration, and Prometheus verification gates passing under ASan/UBSan."

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Zero-Preseed Rate Limiter & Telemetry Initialization and Reset (Priority: P1) 🎯 MVP

As a gateway platform operator deploying custom LLM backends (such as local inference clusters or alternative cloud APIs), I want both the rate-limiting subsystem and the telemetry registry to initialize and reset completely empty—without unconditionally pre-populating legacy vendor policies (`groq`, `openrouter`), circuit state gauges, latency histograms, or skip counters—so that unconfigured vendors never consume quota checks or pollute monitoring scrapes with phantom zero-valued metric series.

**Why this priority**: Eliminates hardcoded vendor lock-in across quota initialization and observability. Unconditionally pre-seeding `groq` and `openrouter` causes every deployment—even pure local or single-vendor setups—to carry spurious quota rules and export misleading `groq` / `openrouter` metrics after startup or reset.

**Independent Test**:
1. Construct a fresh rate limiter instance without registering any providers and verify that no rate-limit rules exist for `groq` or `openrouter` (and calls against arbitrary unconfigured names pass without token deduction).
2. Construct and also explicitly reset a telemetry registry without registering any providers, then inspect its Prometheus text exposition and verify zero `groq` or `openrouter` series appear across `circuit_breaker_state`, `upstream_latency_ms`, and `gateway_ratelimit_skips_total`.

**Acceptance Scenarios**:
1. **Given** a freshly initialized rate limiter before any provider catalog entries are applied and without invoking the opt-in preset helper,  
   **When** configured rules for `groq` or `openrouter` are inspected,  
   **Then** zero policies exist for either vendor and all quota checks treat unconfigured providers as unconstrained.
2. **Given** a freshly initialized telemetry registry before any provider registration or completion traffic occurs,  
   **When** Prometheus metrics exposition is generated,  
   **Then** valid metric type and help descriptors are emitted, with zero pre-seeded series for `groq`, `openrouter`, or any other unconfigured provider.
3. **Given** a telemetry registry that previously accumulated circuit state gauges, latency histograms, request counts, and skip counters for active providers,  
   **When** telemetry state is reset,  
   **Then** all per-provider series and counters are cleared completely and zero phantom `groq` or `openrouter` series are recreated automatically.

---

### User Story 2 - Catalog-Driven Provider Activation & Explicit Test Preset Opt-In (Priority: P1)

As an infrastructure operator and test suite maintainer, I want active rate-limit policies and initialized per-provider telemetry series (`circuit_breaker_state`, `upstream_latency_ms`, and `gateway_ratelimit_skips_total`) to be registered exclusively from providers configured in the provider catalog, while keeping an explicit opt-in helper for standalone unit and concurrency tests that specifically test legacy standard policies or pre-populated vendor telemetry, so that production configuration remains the single source of truth without discarding deterministic legacy test coverage.

**Why this priority**: Establishes strict single-source-of-truth coupling between declarative provider configuration, rate-limit enforcement, and observability while keeping standalone module tests self-contained and explicit.

**Independent Test**:
1. Register custom providers (`ollama`, `deepseek`) in the provider catalog attached to a clean rate limiter and clean telemetry registry.
2. Verify rate-limit rules and Prometheus metric series exist exclusively for `ollama` and `deepseek` (and never for `groq` or `openrouter`).
3. Independently invoke the explicit opt-in standard-preset helper in a standalone test harness and confirm standard preset limits activate on demand for legacy verification cases.

**Acceptance Scenarios**:
1. **Given** an environment configuring only custom providers (e.g., `ollama` and `deepseek`) with per-provider quota rules,  
   **When** the provider catalog initializes and attaches to the rate limiter and telemetry registry,  
   **Then** quota policies and initial telemetry series exist solely for `ollama` and `deepseek`.
2. **Given** a configured custom provider registered without any quota rules,  
   **When** traffic completes against it,  
   **Then** telemetry tracks its requests, latency, and circuit state cleanly without initializing unrelated vendor policies.
3. **Given** an isolated unit or concurrency test explicitly invokes the opt-in standard preset helper on a clean rate limiter (or explicitly registers preset providers in telemetry),  
   **When** legacy `groq`/`openrouter` quota checks run,  
   **Then** standard RPM/RPD limits enforce identically to prior behavior.

---

### User Story 3 - Provider-Catalog Pipeline in `gateway_client` & Full Gate Verification (Priority: P2)

As an engineer operating the `gateway_client` command-line tool and CI verification suite, I want `gateway_client` to assemble its completion stack dynamically from the provider catalog, two-tier cache, and quota-aware dynamic model router rather than hardcoding a legacy `groq`/`openrouter` two-tier router wrapped in a single-provider pre-cache limiter, so that CLI execution honors custom multi-provider environments, cache preemption, and accurate dynamic metrics exposition while keeping offline smoke demos and Prometheus CI format gates passing.

**Why this priority**: Completes migration across all user-facing binaries so server and CLI tool share one consistent, provider-agnostic, cache-first pipeline architecture verified by the Prometheus CI gate.

**Independent Test**:
1. Invoke `gateway_client --metrics` with custom environment provider configuration (e.g., `ollama` + `vllm`), and verify completions succeed through the dynamic model router and Prometheus output contains only the configured custom providers.
2. Invoke `gateway_client --metrics`, `--failover-demo`, and `--health` with default (unconfigured) environment, and verify offline defaults run through the catalog-backed cache-first cascade and pass all Prometheus format checks and automated unit/integration test gates cleanly.

**Acceptance Scenarios**:
1. **Given** `gateway_client` runs with custom providers configured via environment variables (`TESSERA_PROVIDERS` or indexed `PROVIDER_<N>_NAME` configuration) or JSON provider configuration,  
   **When** completion requests and `--metrics` output are requested,  
   **Then** requests dispatch via the catalog-backed dynamic model router using cache-first quota enforcement, and exported metrics reflect solely the configured custom providers.
2. **Given** `gateway_client` runs in a clean environment without explicit provider configuration,  
   **When** standard CLI smoke checks (`--health`, `--metrics`, `--failover-demo`, `--message`, `--no-cache`) are executed,  
   **Then** offline catalog defaults initialize safely via the catalog loader so developer smoke workflows and automated Prometheus exposition verification pass out of the box.
3. **Given** the complete test suite (unit, integration, rate-limit concurrency, gateway concurrency, and Prometheus validation script),  
   **When** compiled and executed under memory and undefined-behavior sanitizers,  
   **Then** 100% of test suites and format gates pass cleanly with zero leaks or sanitizer errors.

---

### Edge Cases

- **Unconfigured Provider Read Lookups After Startup/Reset**: Reading circuit state, request counts, latency snapshots, or skip counts for an arbitrary unconfigured provider MUST safely return zeroed/closed default snapshots without mutating internal collections or emitting phantom zero-value metric series in subsequent Prometheus scrapes.
- **Dynamic Recording Without Prior Registration**: When an active request records a completion count, latency observation, circuit transition, or rate-limit skip for a provider name that was not pre-registered, telemetry MUST create and expose that provider's series on first active observation without fabricating entries for unrelated vendors.
- **Empty Provider Catalog State**: When zero providers are configured (default offline provider seeding is disabled and no provider environment variables are set), generating Prometheus text output MUST emit valid metric headers and cache counters cleanly without errors, panics, or phantom provider series.
- **Cache-First Preservation in `gateway_client`**: Sending cacheable repeat prompts in `gateway_client` MUST resolve via cache ahead of router rate-limit evaluation; sending `--no-cache` MUST bypass cache reads while respecting per-tier quotas inside the cascade.
- **Legacy Diagnostic Flags (`--groq-key`, `--openrouter-key`, `--failover-demo`) in `gateway_client`**: Explicit CLI diagnostic flags MUST continue working seamlessly against catalog-registered providers (applying CLI overrides cleanly and triggering simulated failover across multi-tier routes when two or more tiers exist, or failing clearly with diagnostic context when only a single provider is configured) without breaking custom-provider environments when those flags are absent.
- **Repeated Rate-Limiter Rule Reloads**: Re-registering or attaching a rate limiter to a catalog containing existing rules MUST update provider rules idempotently without duplicating rules or leaking prior preset rules.

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST initialize the distributed rate limiter with zero pre-seeded provider rules unless an explicit opt-in helper is invoked or rules are loaded from configuration.
- **FR-002**: System MUST initialize and reset the telemetry registry with zero pre-seeded provider-specific series (circuit state gauges, upstream latency histograms, request counters, and rate-limit skip counters) for `groq`, `openrouter`, or any other hardcoded vendor.
- **FR-003**: System MUST provide an explicit opt-in helper (separate from default construction/reset) to load standard legacy vendor rate-limit policies (`groq` and `openrouter`) and preset test defaults when explicitly requested by standalone unit tests or compatibility callers.
- **FR-004**: System MUST register rate-limit rules and initial per-provider telemetry series (circuit state gauge, upstream latency histogram, rate-limit skip counter) exclusively for providers configured via the provider catalog.
- **FR-005**: System MUST support read-only telemetry queries for unconfigured providers cleanly without creating persistent phantom entries in exported Prometheus metrics output.
- **FR-006**: System MUST preserve lazy on-demand metric series creation when actual completion traffic, circuit state transitions, latency measurements, or rate-limit skips are actively recorded for any provider.
- **FR-007**: `gateway_client` MUST construct its completion stack from the provider catalog, distributed rate limiter (fed exclusively by catalog rules), quota-aware dynamic model router, and cache-first two-tier cache wrapper—removing the hardcoded legacy dual-provider failover wrapper and outer pre-cache single-provider limiter.
- **FR-008**: `gateway_client` MUST retain full support for its existing operational options (`--health`, `--metrics`, `--failover-demo`, `--message`, `--model`, `--no-cache`, Redis/cache flags, and optional vendor key/model overrides) using offline mock catalog defaults only when zero explicit provider configuration exists.
- **FR-009**: `gateway_client` execution statistics printed to console MUST summarize dynamic router/cascade stats generically across active routes rather than referencing hardcoded single-vendor failover or circuit labels.
- **FR-010**: System MUST update or migrate existing tests and verification scripts whose assertions implicitly relied on unconditional `groq`/`openrouter` pre-seeding so every suite exercises explicit opt-in helpers or catalog registration while keeping Prometheus exposition checks valid.
- **FR-011**: System MUST compile cleanly without warnings under strict compiler flags and pass all unit, integration, concurrency, and Prometheus validation gates with zero memory leaks or undefined behavior under sanitizers.

### Key Entities

- **Provider Catalog (`ProviderRegistry`)**: Authoritative configuration source defining active upstream providers, endpoints, credentials, default models, failover priority order, circuit breaker parameters, and rate-limit rules; responsible for activating corresponding entries in the rate limiter and telemetry registry upon registration.
- **Rate Limiter Store (`DistributedRateLimiter`)**: Provider-indexed policy engine holding zero default vendor rules on fresh construction and populated strictly via explicit rule registration, catalog attachment, or an explicit standalone preset helper.
- **Telemetry Registry (`TelemetryRegistry`)**: Thread-safe observability store holding cache hit counters cleanly at zero and creating provider-labeled gauges, histograms, request counters, and skip counters solely through explicit provider registration or active metric recording.
- **CLI Client Pipeline (`gateway_client`)**: Cache-first completion stack built dynamically from the configured provider catalog and model router, supporting custom multi-vendor deployments and offline smoke/failover diagnostics.

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: **Zero Phantom Pre-Seeded Policies**: 100% of freshly instantiated rate limiter instances contain 0 rules for `groq`, `openrouter`, or any other vendor prior to catalog population or explicit opt-in helper invocation.
- **SC-002**: **Zero Phantom Telemetry Series**: Generating Prometheus exposition output immediately after initialization or telemetry reset (with no providers registered) produces 0 provider-labeled series (`circuit_breaker_state`, `upstream_latency_ms_*`, `gateway_requests_total`, `gateway_ratelimit_skips_total`) for `groq`, `openrouter`, or any unconfigured provider.
- **SC-003**: **Exact Catalog-to-Telemetry Parity**: For any deployment configuring $K$ custom providers ($K \ge 1$, excluding `groq` and `openrouter`), 100% of registered rate-limit rule providers and initialized Prometheus `circuit_breaker_state` / `upstream_latency_ms` / `gateway_ratelimit_skips_total` series correspond exclusively to those $K$ configured providers (0 unexpected vendors).
- **SC-004**: **Unified Catalog-Backed CLI Behavior**: `gateway_client` operates cleanly against both custom provider configurations and zero-config offline defaults, delivering valid Prometheus exposition output and preserving cache-first zero-quota replays across configured providers.
- **SC-005**: **Full Regression & Sanitizer Cleanliness**: 100% of unit tests, integration tests, multi-threaded rate-limit/gateway concurrency tests, and Prometheus format verification checks pass cleanly with 0 compiler warnings and 0 AddressSanitizer or UndefinedBehaviorSanitizer errors.

---

## Assumptions

- Offline fallback catalog defaults in the environment configuration loader (when explicitly permitted by its fallback flag and zero explicit providers or credentials exist) continue providing default mock providers so developer smoke runs and default Prometheus validation scripts work without requiring external API keys or manual environment setup.
- Existing tests that explicitly target legacy `groq` and `openrouter` behavior (e.g., standard policy RPM checks or multi-window concurrency tests) will explicitly invoke the preserved opt-in helper or register those providers explicitly, rather than expecting every clean instance to bundle them implicitly.
- Dynamic lazy series creation on active metric recording (request completion counts, circuit state updates, upstream latency observations, or rate-limit skips) remains preserved so runtime telemetry remains resilient regardless of initialization order when events are recorded directly.
- Non-mutating read queries for circuit state, request counts, rate-limit skip counts, or upstream latency snapshots return safe zeroed/closed defaults for unknown providers without inserting phantom keys into exported telemetry maps.
- Unkeyed endpoints in `gateway_client` (including custom or local endpoints configured via environment variables) default to simulated offline execution for hermetic CLI testing unless API credentials or an explicit live-endpoint CLI option are supplied.
