#include "tessera/ratelimit/rate_limited_backend.hpp"
#include "tessera/common/logger.hpp"
#include <algorithm>

namespace tessera {

RateLimitedBackend::RateLimitedBackend(
    std::shared_ptr<ICompletionBackend> inner_backend,
    std::shared_ptr<DistributedRateLimiter> rate_limiter,
    std::string default_provider)
    : inner_backend_(std::move(inner_backend)),
      rate_limiter_(std::move(rate_limiter)),
      default_provider_(std::move(default_provider)) {}

std::string RateLimitedBackend::ResolveProvider(
    std::string_view model,
    const std::string& default_provider) {
  std::string lower;
  lower.reserve(model.size());
  for (char c : model) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }

  if (lower.find("openrouter") != std::string::npos || lower.find("gemma") != std::string::npos) {
    return "openrouter";
  }
  if (lower.find("groq") != std::string::npos || lower.find("llama") != std::string::npos) {
    return "groq";
  }
  return default_provider;
}

Status RateLimitedBackend::Complete(
    const ChatCompletionRequest& request,
    ChatCompletionResponse* response) {
  if (response == nullptr) {
    return Status::InvalidArgument("Response pointer cannot be null");
  }

  std::string provider = ResolveProvider(request.model, default_provider_);

  if (rate_limiter_ != nullptr) {
    auto res = rate_limiter_->CheckAndConsume(provider);
    if (!res.allowed) {
      TESSERA_LOG_WARN("RateLimitedBackend: rejected request for provider '{}'. Retry after {}ms",
                       provider, res.retry_after_ms);
      return Status::ResourceExhausted(
          "Rate limit exceeded for provider '" + provider + "'. Retry after " +
          std::to_string(res.retry_after_ms) + "ms");
    }
  }

  if (inner_backend_ == nullptr) {
    return Status::Internal("No inner backend configured in RateLimitedBackend");
  }

  return inner_backend_->Complete(request, response);
}

}  // namespace tessera

