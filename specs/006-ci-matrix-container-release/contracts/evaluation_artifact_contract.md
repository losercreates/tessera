# Evaluation Artifact Contract: JSON Schema

**Feature**: `006-ci-matrix-container-release`  
**Date**: 2026-11-14  
**Status**: Completed  

---

## 1. File Path & Exposition

- **Format**: JSON Lines / Pretty JSON
- **Default Filename**: `eval_report.json`
- **Exposition**: Uploaded as a workflow artifact named `evaluation-report-<compiler>` via `actions/upload-artifact@v4`.

---

## 2. JSON Schema Definition

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "EvaluationBenchmarkReport",
  "type": "object",
  "required": [
    "total_queries",
    "successful_queries",
    "failed_queries",
    "tier1_exact_hits",
    "tier2_semantic_hits",
    "cache_misses",
    "cache_hit_ratio_pct",
    "cold_p95_latency_ms",
    "warm_p95_latency_ms",
    "p95_latency_reduction_pct",
    "token_savings_pct",
    "throughput_rps",
    "max_rss_mb",
    "p50_latency_ms",
    "p90_latency_ms",
    "p95_latency_ms",
    "p99_latency_ms",
    "total_duration_seconds"
  ],
  "properties": {
    "total_queries": {
      "type": "integer",
      "minimum": 1
    },
    "successful_queries": {
      "type": "integer",
      "minimum": 0
    },
    "failed_queries": {
      "type": "integer",
      "maximum": 0
    },
    "tier1_exact_hits": {
      "type": "integer",
      "minimum": 0
    },
    "tier2_semantic_hits": {
      "type": "integer",
      "minimum": 0
    },
    "cache_misses": {
      "type": "integer",
      "minimum": 0
    },
    "cache_hit_ratio_pct": {
      "type": "number",
      "minimum": 0.0,
      "maximum": 100.0
    },
    "cold_p95_latency_ms": {
      "type": "number",
      "minimum": 0.0
    },
    "warm_p95_latency_ms": {
      "type": "number",
      "minimum": 0.0
    },
    "p95_latency_reduction_pct": {
      "type": "number",
      "minimum": 80.0,
      "maximum": 100.0
    },
    "token_savings_pct": {
      "type": "number",
      "minimum": 25.0,
      "maximum": 100.0
    },
    "throughput_rps": {
      "type": "number",
      "minimum": 0.0
    },
    "max_rss_mb": {
      "type": "number",
      "maximum": 500.0
    },
    "p50_latency_ms": { "type": "number" },
    "p90_latency_ms": { "type": "number" },
    "p95_latency_ms": { "type": "number" },
    "p99_latency_ms": { "type": "number" },
    "total_duration_seconds": { "type": "number" }
  }
}
```

