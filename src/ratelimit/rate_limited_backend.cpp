#include "tessera/ratelimit/rate_limited_backend.hpp"
#include "tessera/common/logger.hpp"
#include <algorithm>

namespace tessera {

RateLimitedBackend::RateLimitedBackend(
    std::shared_ptr<ICompletionBackend> inner_backend,
    std::shared_ptr<DistributedRateLimiter> rate_limiter,
    std::string default_provider,
    ProviderResolver resolver)
    : inner_backend_(std::move(inner_backend)),
      rate_limiter_(std::move(rate_limiter)),
      default_provider_(std::move(default_provider)),
      resolver_(std::move(resolver)) {}

std::string RateLimitedBackend::ResolveProvider(
    std::string_view model,
    const std::string& default_provider) {
  if (model.empty()) {
    return default_provider;
  }

  std::string lower;
  lower.reserve(model.size());
  for (char c : model) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }

  // 1. Check if model has provider prefix syntax: "<provider>/<model>"
  auto slash_pos = lower.find('/');
  if (slash_pos != std::string::npos && slash_pos > 0) {
    std::string prefix = lower.substr(0, slash_pos);
    if (prefix == "openrouter") {
      return "openrouter";
    }
    if (prefix != "google" && prefix != "anthropic" && prefix != "meta" && prefix != "mistralai") {
      return prefix;
    }
  }

  // 2. Known model substring resolution
  if (lower.find("ollama") != std::string::npos) {
    return "ollama";
  }
  if (lower.find("openrouter") != std::string::npos || lower.find("gemma") != std::string::npos) {
    return "openrouter";
  }
  if (lower.find("groq") != std::string::npos || lower.find("llama") != std::string::npos) {
    return "groq";
  }
  if (lower.find("deepseek") != std::string::npos) {
    return "deepseek";
  }
  if (lower.find("mistral") != std::string::npos) {
    return "mistral";
  }
  if (lower.find("gpt") != std::string::npos || lower.find("openai") != std::string::npos) {
    return "openai";
  }

  return default_provider;
}

Status RateLimitedBackend::Complete(
    const ChatCompletionRequest& request,
    ChatCompletionResponse* response) {
  if (response == nullptr) {
    return Status::InvalidArgument("Response pointer cannot be null");
  }

  std::string provider;
  if (resolver_) {
    provider = resolver_(request.model);
  }
  if (provider.empty()) {
    provider = ResolveProvider(request.model, default_provider_);
  }

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

