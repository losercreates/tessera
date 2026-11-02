#pragma once

#include "tessera/common/types.hpp"
#include <string>
#include <string_view>

namespace tessera {

class Sha256 {
 public:
  // Compute SHA-256 hash of a string, returning a 64-character lowercase hex string
  static std::string Hash(std::string_view input);

  // Serialize ChatCompletionRequest into a canonical deterministic string
  static std::string ComputeCanonicalString(const ChatCompletionRequest& req);

  // Generate Tier 1 exact match cache key: "cache:exact:<sha256_hash>"
  static std::string GetExactCacheKey(const ChatCompletionRequest& req);
};

}  // namespace tessera

