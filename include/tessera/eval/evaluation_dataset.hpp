#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tessera {

struct EvaluationQuery {
  uint32_t id{0};
  std::string prompt;
  bool is_paraphrase{false};
  std::optional<uint32_t> base_query_id;
  std::string category;
  std::string expected_tier{"upstream"};
};

class EvaluationDataset {
 public:
  /**
   * Generates a deterministic evaluation workload dataset.
   * Default: 1,000 queries with 30% semantically equivalent paraphrased duplicates
   * (700 base queries + 300 paraphrased queries).
   */
  [[nodiscard]] static std::vector<EvaluationQuery> GenerateDataset(
      size_t total_queries = 1000,
      double paraphrase_ratio = 0.30);
};

}  // namespace tessera

