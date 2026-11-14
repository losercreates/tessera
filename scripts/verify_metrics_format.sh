#!/usr/bin/env bash
# ==============================================================================
# Tessera Gateway Prometheus Metrics Exposition Validator
# Asserts that metrics exposition output adheres to Prometheus 0.0.4 standards
# ==============================================================================

set -euo pipefail

METRICS_FILE="${1:-}"
TEMP_FILE=""

if [ -n "$METRICS_FILE" ] && [ -x "$METRICS_FILE" ] && [ ! -d "$METRICS_FILE" ]; then
    CLIENT_BIN="$METRICS_FILE"
    TEMP_FILE=$(mktemp)
    trap 'rm -f "$TEMP_FILE"' EXIT
    echo "Querying executable $CLIENT_BIN --metrics..."
    "$CLIENT_BIN" --message "Metrics validation test" --metrics > "$TEMP_FILE" 2>&1 || true
    METRICS_FILE="$TEMP_FILE"
elif [ -z "$METRICS_FILE" ] || [ ! -f "$METRICS_FILE" ]; then
    TEMP_FILE=$(mktemp)
    trap 'rm -f "$TEMP_FILE"' EXIT
    
    CLIENT_BIN=""
    if [ -x "./build/gateway_client" ]; then
        CLIENT_BIN="./build/gateway_client"
    elif [ -x "./build-ci/gateway_client" ]; then
        CLIENT_BIN="./build-ci/gateway_client"
    else
        echo "[FAIL] gateway_client executable not found in ./build or ./build-ci" >&2
        exit 1
    fi
    
    echo "Querying $CLIENT_BIN --metrics..."
    "$CLIENT_BIN" --message "Metrics validation test" --metrics > "$TEMP_FILE" 2>&1 || true
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

echo "-----------------------------------------------------------------"

if [ "$FAILED" -eq 0 ]; then
    echo ">> All Prometheus metrics exposition format checks PASSED cleanly."
    exit 0
else
    echo "[CI GATE BREACH] One or more Prometheus metrics format checks FAILED." >&2
    exit 1
fi

