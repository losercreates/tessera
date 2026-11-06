#pragma once

#include "tessera/backend/completion_backend.hpp"
#include "tessera/eval/evaluation_dataset.hpp"
#include <memory>
#include <string>
#include <vector>

namespace tessera {

struct EvaluationConfig {
  size_t query_count = 1000;
  double paraphrase_ratio = 0.30;
  size_t concurrency = 8;
  bool verbose = false;
  std::string output_json_path;
};

struct EvaluationReport {
  uint32_t total_queries = 0;
  uint32_t successful_queries = 0;
  uint32_t failed_queries = 0;

  uint32_t tier1_exact_hits = 0;
  uint32_t tier2_semantic_hits = 0;
  uint32_t cache_misses = 0;
  double cache_hit_ratio_pct = 0.0;

  double p50_latency_ms = 0.0;
  double p90_latency_ms = 0.0;
  double p95_latency_ms = 0.0;
  double p99_latency_ms = 0.0;

  double cold_p95_latency_ms = 0.0;
  double warm_p95_latency_ms = 0.0;
  double p95_latency_reduction_pct = 0.0;
  double token_savings_pct = 0.0;

  double throughput_rps = 0.0;
  double total_duration_seconds = 0.0;
  double max_rss_mb = 0.0;
};

class EvaluationRunner {
 public:
  explicit EvaluationRunner(std::shared_ptr<ICompletionBackend> backend);

  [[nodiscard]] EvaluationReport Run(const EvaluationConfig& config = {});

  [[nodiscard]] static std::string FormatMarkdownReport(const EvaluationReport& report);
  [[nodiscard]] static std::string FormatJsonReport(const EvaluationReport& report);

  // Reads Linux /proc/self/statm resident set size in megabytes
  [[nodiscard]] static double GetResidentMemoryMB();

 private:
  std::shared_ptr<ICompletionBackend> backend_;
};

}  // namespace tessera

