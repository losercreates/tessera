#!/usr/bin/env bash
# ==============================================================================
# Tessera Gateway Prometheus Metrics Exposition Validator
# Asserts that metrics exposition output adheres to Prometheus 0.0.4 standards
# ==============================================================================

set -euo pipefail

METRICS_FILE="${1:-}"
TEMP_FILE=""
SERVER_PID=""
SERVER_LOG=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [ -n "$TEMP_FILE" ]; then
        rm -f "$TEMP_FILE"
    fi
    if [ -n "$SERVER_LOG" ]; then
        rm -f "$SERVER_LOG"
    fi
}
trap cleanup EXIT

if [ -n "$METRICS_FILE" ] && [ -f "$METRICS_FILE" ] && [ ! -x "$METRICS_FILE" ]; then
    # Static metrics file provided explicitly
    true
else
    CLIENT_BIN=""
    if [ -n "$METRICS_FILE" ] && [ -x "$METRICS_FILE" ] && [ ! -d "$METRICS_FILE" ]; then
        CLIENT_BIN="$METRICS_FILE"
    elif [ -x "./build/gateway_client" ]; then
        CLIENT_BIN="./build/gateway_client"
    elif [ -x "./build-ci/gateway_client" ]; then
        CLIENT_BIN="./build-ci/gateway_client"
    else
        echo "[FAIL] gateway_client executable not found in ./build or ./build-ci" >&2
        exit 1
    fi

    TEMP_FILE=$(mktemp)
    GW_HOST="${GATEWAY_HOST:-127.0.0.1}"
    GW_PORT="${GATEWAY_PORT:-50051}"
    GW_HTTP_PORT="${HTTP_PORT:-8080}"
    GW_METRICS_PORT="${METRICS_PORT:-9090}"

    if ! "$CLIENT_BIN" --host="$GW_HOST" --port="$GW_PORT" --health >/dev/null 2>&1; then
        SERVER_BIN="$(dirname "$CLIENT_BIN")/gateway_server"
        if [ ! -x "$SERVER_BIN" ]; then
            echo "[FAIL] No gateway server listening on $GW_HOST:$GW_PORT and $SERVER_BIN not found" >&2
            exit 1
        fi

        if command -v python3 >/dev/null 2>&1; then
            read -r GW_PORT GW_HTTP_PORT GW_METRICS_PORT <<EOF
$(python3 -c 'import socket
socks = [socket.socket() for _ in range(3)]
for s in socks: s.bind(("127.0.0.1", 0))
print(" ".join(str(s.getsockname()[1]) for s in socks))
for s in socks: s.close()')
EOF
        else
            GW_PORT=15051
            GW_HTTP_PORT=18080
            GW_METRICS_PORT=19090
        fi
        GW_HOST="127.0.0.1"
        SERVER_LOG=$(mktemp)

        echo "Starting temporary $SERVER_BIN (--mock-redis --mock-upstreams) on ports rpc=$GW_PORT http=$GW_HTTP_PORT metrics=$GW_METRICS_PORT..."
        "$SERVER_BIN" \
            --host="$GW_HOST" \
            --port="$GW_PORT" \
            --http-port="$GW_HTTP_PORT" \
            --metrics-port="$GW_METRICS_PORT" \
            --mock-redis \
            --mock-upstreams \
            --threads=2 >"$SERVER_LOG" 2>&1 &
        SERVER_PID=$!

        READY=0
        for _ in $(seq 1 50); do
            if "$CLIENT_BIN" --host="$GW_HOST" --port="$GW_PORT" --health >/dev/null 2>&1; then
                READY=1
                break
            fi
            if ! kill -0 "$SERVER_PID" 2>/dev/null; then
                echo "[FAIL] Temporary gateway_server exited unexpectedly:" >&2
                cat "$SERVER_LOG" >&2 || true
                exit 1
            fi
            sleep 0.1
        done
        if [ "$READY" -ne 1 ]; then
            echo "[FAIL] Timed out waiting for temporary gateway_server to become healthy" >&2
            cat "$SERVER_LOG" >&2 || true
            exit 1
        fi
    fi

    echo "Querying $CLIENT_BIN (--host=$GW_HOST --port=$GW_PORT --metrics-port=$GW_METRICS_PORT) --metrics..."
    "$CLIENT_BIN" --host="$GW_HOST" --port="$GW_PORT" --http-port="$GW_HTTP_PORT" --http --health >/dev/null
    "$CLIENT_BIN" \
        --host="$GW_HOST" \
        --port="$GW_PORT" \
        --metrics-port="$GW_METRICS_PORT" \
        --message "Metrics validation test" \
        --metrics > "$TEMP_FILE"
    METRICS_FILE="$TEMP_FILE"
fi

echo "================================================================="
echo " Tessera Prometheus Metrics Exposition Verification"
echo "================================================================="

FAILED=0

check_pattern() {
    local pattern="$1"
    local desc="$2"
    if grep -qE "$pattern" "$METRICS_FILE"; then
        printf "  [✓] %-48s [PASS]\n" "$desc"
    else
        printf "  [!] %-48s [FAIL]\n" "$desc"
        FAILED=1
    fi
}

# 1. gateway_requests_total
check_pattern "# HELP gateway_requests_total" "HELP gateway_requests_total descriptor"
check_pattern "# TYPE gateway_requests_total counter" "TYPE gateway_requests_total counter declaration"
check_pattern "gateway_requests_total\{.*provider=.*status=.*\}" "gateway_requests_total labeled sample"

# 2. gateway_cache_hits_total
check_pattern "# HELP gateway_cache_hits_total" "HELP gateway_cache_hits_total descriptor"
check_pattern "# TYPE gateway_cache_hits_total counter" "TYPE gateway_cache_hits_total counter declaration"
check_pattern "gateway_cache_hits_total\{tier=\".*\"\}" "gateway_cache_hits_total tier sample"

# 3. circuit_breaker_state
check_pattern "# HELP circuit_breaker_state" "HELP circuit_breaker_state descriptor"
check_pattern "# TYPE circuit_breaker_state gauge" "TYPE circuit_breaker_state gauge declaration"
check_pattern "circuit_breaker_state\{provider=\".*\"\} [0-2]" "circuit_breaker_state gauge sample"

# 4. upstream_latency_ms
check_pattern "# HELP upstream_latency_ms" "HELP upstream_latency_ms descriptor"
check_pattern "# TYPE upstream_latency_ms histogram" "TYPE upstream_latency_ms histogram declaration"
check_pattern "upstream_latency_ms_bucket\{provider=\".*\",le=\".*\"\}" "upstream_latency_ms histogram bucket sample"
check_pattern "upstream_latency_ms_sum\{provider=\".*\"\}" "upstream_latency_ms histogram sum sample"
check_pattern "upstream_latency_ms_count\{provider=\".*\"\}" "upstream_latency_ms histogram count sample"

# 5. gateway_ratelimit_skips_total
check_pattern "# HELP gateway_ratelimit_skips_total" "HELP gateway_ratelimit_skips_total descriptor"
check_pattern "# TYPE gateway_ratelimit_skips_total counter" "TYPE gateway_ratelimit_skips_total counter declaration"

# 6. Dynamic per-provider verification
PROVIDERS_TO_VERIFY="${2:-${TESSERA_PROVIDERS:-}}"

if [ -n "$PROVIDERS_TO_VERIFY" ]; then
    echo "--- Verifying explicit providers: $PROVIDERS_TO_VERIFY ---"
    IFS=',' read -ra PROV_ARRAY <<< "$PROVIDERS_TO_VERIFY"
    for prov in "${PROV_ARRAY[@]}"; do
        prov=$(echo "$prov" | tr -d '[:space:]')
        if [ -n "$prov" ]; then
            check_pattern "circuit_breaker_state\{provider=\"$prov\"\} [0-2]" "circuit_breaker_state for provider '$prov'"
            check_pattern "upstream_latency_ms_count\{provider=\"$prov\"\}" "upstream_latency_ms_count for provider '$prov'"
            check_pattern "upstream_latency_ms_bucket\{provider=\"$prov\",le=\".*\"\}" "upstream_latency_ms_bucket for provider '$prov'"
        fi
    done
fi

# Also discover and validate any dynamic providers present in the metrics payload
DISCOVERED_PROVIDERS=$(grep -oE 'circuit_breaker_state\{provider="[^"]+"\}' "$METRICS_FILE" | sed -E 's/.*provider="([^"]+)".*/\1/' | sort -u || true)
if [ -n "$DISCOVERED_PROVIDERS" ]; then
    echo "--- Validating auto-discovered providers from payload ---"
    for prov in $DISCOVERED_PROVIDERS; do
        check_pattern "circuit_breaker_state\{provider=\"$prov\"\} [0-2]" "valid gauge state for '$prov'"
        check_pattern "upstream_latency_ms_count\{provider=\"$prov\"\}" "valid latency count for '$prov'"
    done
fi

echo "-----------------------------------------------------------------"

if [ "$FAILED" -eq 0 ]; then
    echo ">> All Prometheus metrics exposition format checks PASSED cleanly."
    exit 0
else
    echo "[CI GATE BREACH] One or more Prometheus metrics format checks FAILED." >&2
    exit 1
fi

