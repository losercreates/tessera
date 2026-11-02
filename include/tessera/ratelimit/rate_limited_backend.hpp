#pragma once

#include "tessera/backend/completion_backend.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace tessera {

class RateLimitedBackend : public ICompletionBackend {
 public:
  RateLimitedBackend(
      std::shared_ptr<ICompletionBackend> inner_backend,
      std::shared_ptr<DistributedRateLimiter> rate_limiter,
      std::string default_provider = "groq");

  Status Complete(const ChatCompletionRequest& request, ChatCompletionResponse* response) override;

  [[nodiscard]] std::string_view Name() const noexcept override {
    return "RateLimitedBackend";
  }

  static std::string ResolveProvider(std::string_view model, const std::string& default_provider = "groq");

 private:
  std::shared_ptr<ICompletionBackend> inner_backend_;
  std::shared_ptr<DistributedRateLimiter> rate_limiter_;
  std::string default_provider_;
};

}  // namespace tessera

