#include "tessera/routing/model_router.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include "tessera/common/logger.hpp"
#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace tessera {

ModelRouter::ModelRouter() = default;

ModelRouter::ModelRouter(std::shared_ptr<CascadeRouter> default_cascade)
    : default_cascade_(std::move(default_cascade)) {}

void ModelRouter::RegisterRoute(std::string pattern, std::shared_ptr<CascadeRouter> cascade) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pattern.size() > 2 && pattern.substr(pattern.size() - 2) == "/*") {
    std::string prefix = pattern.substr(0, pattern.size() - 2);
    wildcard_routes_[prefix] = std::move(cascade);
    TESSERA_LOG_INFO("ModelRouter: registered wildcard route '{}/ *'", prefix);
  } else {
    exact_routes_[pattern] = std::move(cascade);
    TESSERA_LOG_INFO("ModelRouter: registered exact route '{}'", pattern);
  }
}

void ModelRouter::RegisterRoute(std::string pattern, std::vector<CascadeTarget> targets) {
  std::shared_ptr<DistributedRateLimiter> limiter;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    limiter = rate_limiter_;
  }
  auto cascade = std::make_shared<CascadeRouter>(std::move(targets), pattern, std::move(limiter));
  RegisterRoute(std::move(pattern), std::move(cascade));
}

void ModelRouter::SetDefaultCascade(std::shared_ptr<CascadeRouter> cascade) {
  std::lock_guard<std::mutex> lock(mutex_);
  default_cascade_ = std::move(cascade);
}

std::shared_ptr<CascadeRouter> ModelRouter::GetDefaultCascade() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return default_cascade_;
}

void ModelRouter::SetRateLimiter(std::shared_ptr<DistributedRateLimiter> rate_limiter) {
  std::lock_guard<std::mutex> lock(mutex_);
  rate_limiter_ = std::move(rate_limiter);
  if (default_cascade_) {
    default_cascade_->SetRateLimiter(rate_limiter_);
  }
  for (auto& [_, cascade] : exact_routes_) {
    if (cascade) cascade->SetRateLimiter(rate_limiter_);
  }
  for (auto& [_, cascade] : wildcard_routes_) {
    if (cascade) cascade->SetRateLimiter(rate_limiter_);
  }
}

std::shared_ptr<CascadeRouter> ModelRouter::ResolveCascade(
    std::string_view model,
    std::string* remapped_model) const {
  std::lock_guard<std::mutex> lock(mutex_);

  if (model.empty()) {
    return default_cascade_;
  }

  std::string model_str(model);

  // 1. Exact match lookup
  auto it = exact_routes_.find(model_str);
  if (it != exact_routes_.end()) {
    return it->second;
  }

  // 2. Wildcard prefix matching (<provider>/<model>)
  auto slash_pos = model.find('/');
  if (slash_pos != std::string_view::npos && slash_pos > 0) {
    std::string prefix(model.substr(0, slash_pos));
    auto wit = wildcard_routes_.find(prefix);
    if (wit != wildcard_routes_.end()) {
      if (remapped_model) {
        *remapped_model = std::string(model.substr(slash_pos + 1));
      }
      return wit->second;
    }
  }

  // 3. Fallback to default cascade
  return default_cascade_;
}

Status ModelRouter::Complete(
    const ChatCompletionRequest& request,
    ChatCompletionResponse* response) {
  if (response == nullptr) {
    return Status::InvalidArgument("Response pointer cannot be null");
  }

  std::string remapped_model;
  auto cascade = ResolveCascade(request.model, &remapped_model);

  if (cascade == nullptr) {
    auto available = GetRegisteredModels();
    std::ostringstream oss;
    oss << "Unknown or unmapped model identifier '" << request.model << "'. Available models: [";
    for (size_t i = 0; i < available.size(); ++i) {
      oss << available[i];
      if (i + 1 < available.size()) oss << ", ";
    }
    oss << "]";
    return Status::InvalidArgument(oss.str());
  }

  if (!remapped_model.empty()) {
    ChatCompletionRequest modified_req = request;
    modified_req.model = remapped_model;
    return cascade->Complete(modified_req, response);
  }

  return cascade->Complete(request, response);
}

std::vector<std::string> ModelRouter::GetRegisteredModels() const {
  std::vector<std::string> models;
  std::lock_guard<std::mutex> lock(mutex_);
  models.reserve(exact_routes_.size() + wildcard_routes_.size());
  for (const auto& [name, _] : exact_routes_) {
    models.push_back(name);
  }
  for (const auto& [prefix, _] : wildcard_routes_) {
    models.push_back(prefix + "/*");
  }
  std::sort(models.begin(), models.end());
  return models;
}

size_t ModelRouter::RouteCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return exact_routes_.size() + wildcard_routes_.size();
}

bool ModelRouter::DetectCycle(
    const std::unordered_map<std::string, std::string>& aliases,
    std::string* cycle_desc) {
  for (const auto& [start_node, _] : aliases) {
    std::unordered_set<std::string> visited;
    std::string curr = start_node;

    while (true) {
      if (visited.find(curr) != visited.end()) {
        if (cycle_desc) {
          *cycle_desc = "Cycle detected involving alias '" + curr + "'";
        }
        return true;
      }
      visited.insert(curr);

      auto it = aliases.find(curr);
      if (it == aliases.end()) {
        break;
      }
      curr = it->second;
    }
  }
  return false;
}

std::shared_ptr<ModelRouter> ModelRouter::CreateFromRegistry(
    const ProviderRegistry& registry,
    std::shared_ptr<DistributedRateLimiter> rate_limiter) {
  auto router = std::make_shared<ModelRouter>();
  router->SetRateLimiter(rate_limiter);

  // Build default cascade: all registered providers in exact configured order
  std::vector<CascadeTarget> default_targets;
  auto providers = registry.GetRegisteredProviders();

  for (size_t i = 0; i < providers.size(); ++i) {
    const auto& prov = providers[i];
    auto client = registry.GetProviderClient(prov);
    auto breaker = registry.GetProviderBreaker(prov);
    const auto* pcfg = registry.GetProviderConfig(prov);
    if (!client || !breaker || !pcfg) continue;

    default_targets.push_back(CascadeTarget{
        .client = client,
        .breaker = breaker,
        .target_model = pcfg->default_model,
        .priority = static_cast<uint32_t>(i),
    });
  }

  if (!default_targets.empty()) {
    router->SetDefaultCascade(
        std::make_shared<CascadeRouter>(default_targets, "default", rate_limiter));
  }

  // Register dedicated routes per provider
  for (const auto& prov : providers) {
    auto client = registry.GetProviderClient(prov);
    auto breaker = registry.GetProviderBreaker(prov);
    const auto* pcfg = registry.GetProviderConfig(prov);

    if (!client || !breaker || !pcfg) continue;

    // Single provider target
    std::vector<CascadeTarget> single_target = {
        CascadeTarget{.client = client, .breaker = breaker, .target_model = "", .priority = 0}};
    auto prov_cascade = std::make_shared<CascadeRouter>(single_target, prov, rate_limiter);

    // Register wildcard prefix: <provider>/*
    router->RegisterRoute(prov + "/*", prov_cascade);

    // Register default model and all supported models as exact matches
    if (!pcfg->default_model.empty()) {
      router->RegisterRoute(pcfg->default_model, prov_cascade);
    }
    for (const auto& m : pcfg->supported_models) {
      router->RegisterRoute(m, prov_cascade);
    }
  }

  return router;
}

}  // namespace tessera
