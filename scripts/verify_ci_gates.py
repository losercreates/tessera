#!/usr/bin/env python3
"""
Tessera Gateway CI SLA Gate Verification Script
Validates that eval_report.json strictly satisfies all performance,
latency reduction, token cost savings, and memory boundaries.
"""

import sys
import json
import os

def main():
    if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
        print("Usage: python3 scripts/verify_ci_gates.py [path/to/eval_report.json]")
        sys.exit(0)

    report_path = sys.argv[1] if len(sys.argv) > 1 else "eval_report.json"

    if not os.path.exists(report_path):
        print(f"[FAIL] Evaluation report file not found at: {report_path}", file=sys.stderr)
        sys.exit(1)

    try:
        with open(report_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:
        print(f"[FAIL] Failed to parse JSON report at {report_path}: {e}", file=sys.stderr)
        sys.exit(1)

    print("=" * 65)
    print(" Tessera Gateway CI Evaluation SLA Verification")
    print("=" * 65)

    failures = []

    # 1. Total queries check
    total_queries = data.get("total_queries", 0)
    if total_queries < 1000:
        failures.append(f"Total queries ({total_queries}) below expected minimum 1000")
        print(f"  [!] Total Queries:        {total_queries:<10} (Expected: >= 1000) [FAIL]")
    else:
        print(f"  [✓] Total Queries:        {total_queries:<10} (Target: >= 1000)   [PASS]")

    # 2. Failed queries check (Must be strictly 0)
    failed_queries = data.get("failed_queries", 0)
    if failed_queries > 0:
        failures.append(f"Failed queries ({failed_queries}) must be 0")
        print(f"  [!] Failed Queries:       {failed_queries:<10} (Expected: 0)         [FAIL]")
    else:
        print(f"  [✓] Failed Queries:       {failed_queries:<10} (Target: 0)           [PASS]")

    # 3. p95 Latency reduction percentage (Must be >= 80.0%)
    p95_reduction = data.get("p95_latency_reduction_pct", 0.0)
    if p95_reduction < 80.0:
        failures.append(f"p95 latency reduction ({p95_reduction:.1f}%) below target 80.0%")
        print(f"  [!] p95 Latency Reduction:{p95_reduction:>6.1f}%     (Target: >= 80.0%)  [FAIL]")
    else:
        print(f"  [✓] p95 Latency Reduction:{p95_reduction:>6.1f}%     (Target: >= 80.0%)  [PASS]")

    # 4. Token cost savings percentage (Must be >= 25.0%)
    token_savings = data.get("token_savings_pct", 0.0)
    if token_savings < 25.0:
        failures.append(f"Token cost savings ({token_savings:.1f}%) below target 25.0%")
        print(f"  [!] Token Cost Savings:   {token_savings:>6.1f}%     (Target: >= 25.0%)  [FAIL]")
    else:
        print(f"  [✓] Token Cost Savings:   {token_savings:>6.1f}%     (Target: >= 25.0%)  [PASS]")

    # 5. Peak resident memory RSS (Must be < 500.0 MB)
    max_rss = data.get("max_rss_mb", 0.0)
    if max_rss >= 500.0:
        failures.append(f"Peak resident memory ({max_rss:.1f}MB) exceeded limit 500.0MB")
        print(f"  [!] Peak Resident Memory: {max_rss:>6.1f} MB    (Target: < 500.0MB) [FAIL]")
    else:
        print(f"  [✓] Peak Resident Memory: {max_rss:>6.1f} MB    (Target: < 500.0MB) [PASS]")

    print("-" * 65)
    print(f"  Throughput:               {data.get('throughput_rps', 0.0):>8.1f} req/sec")
    print(f"  Cold p95 Latency:         {data.get('cold_p95_latency_ms', 0.0):>8.2f} ms")
    print(f"  Warm p95 Latency:         {data.get('warm_p95_latency_ms', 0.0):>8.2f} ms")
    print(f"  Tier 2 Semantic Hits:     {data.get('tier2_semantic_hits', 0):>8}")
    print("=" * 65)

    if failures:
        print("\n[CI GATE BREACH] The following evaluation SLA gates failed:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        sys.exit(1)
    else:
        print("\n>> All CI evaluation SLA quality gates PASSED cleanly.")
        sys.exit(0)

if __name__ == "__main__":
    main()

