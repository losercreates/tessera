# ==============================================================================
# Tessera Gateway Multi-Stage Production Dockerfile
# Stage 1: Build stage with C++20 toolchain, CMake, Ninja, and OpenSSL
# Stage 2: Minimal runtime image with security hardening (non-root user)
# ==============================================================================

# Stage 1: Builder
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    libssl-dev \
    pkg-config \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy project source tree
COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/
COPY tools/ ./tools/
COPY protos/ ./protos/

# Build optimized release binaries
RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF \
    -DENABLE_SANITIZERS=OFF \
    && cmake --build build --parallel $(nproc)

# ==============================================================================
# Stage 2: Minimal Hardened Runtime
# ==============================================================================
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Create unprivileged service user and group
RUN groupadd -g 10001 tessera && \
    useradd -u 10001 -g tessera -m -s /bin/bash tessera

WORKDIR /app

# Copy binaries from builder stage
COPY --from=builder /src/build/gateway_server /usr/local/bin/gateway_server
COPY --from=builder /src/build/gateway_client /usr/local/bin/gateway_client
COPY --from=builder /src/build/evaluation_harness /usr/local/bin/evaluation_harness
COPY --from=builder /src/build/cache_benchmark /usr/local/bin/cache_benchmark
COPY --from=builder /src/build/semantic_benchmark /usr/local/bin/semantic_benchmark

# Set ownership
RUN chown -R tessera:tessera /app

# Expose gRPC service and Prometheus metrics ports
EXPOSE 50051 9090

# Default environment configuration
ENV REDIS_HOST=redis-stack \
    REDIS_PORT=6379 \
    GATEWAY_PORT=50051 \
    METRICS_PORT=9090 \
    WORKER_THREADS=8 \
    CACHE_TTL=86400 \
    SIMILARITY_THRESHOLD=0.95

USER tessera

HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD /usr/local/bin/gateway_client --health || exit 1

ENTRYPOINT ["/usr/local/bin/gateway_server"]

