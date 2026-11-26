#pragma once

#include "tessera/backend/completion_backend.hpp"
#include "tessera/routing/cascade_router.hpp"
#include "tessera/routing/provider_registry.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tessera {

class DistributedRateLimiter;

class ModelRouter : public ICompletionBackend {
 public:
  ModelRouter();
  explicit ModelRouter(std::shared_ptr<CascadeRouter> default_cascade);
  ~ModelRouter() override = default;

  Status Complete(const ChatCompletionRequest& request, ChatCompletionResponse* response) override;

  [[nodiscard]] std::string_view Name() const noexcept override {
    return "ModelRouter";
  }

  /**
   * Routing configuration
   */
  void RegisterRoute(std::string pattern, std::shared_ptr<CascadeRouter> cascade);
  void RegisterRoute(std::string pattern, std::vector<CascadeTarget> targets);
  void SetDefaultCascade(std::shared_ptr<CascadeRouter> cascade);
  [[nodiscard]] std::shared_ptr<CascadeRouter> GetDefaultCascade() const;
  void SetRateLimiter(std::shared_ptr<DistributedRateLimiter> rate_limiter);

  /**
   * Model resolution
   */
  [[nodiscard]] std::shared_ptr<CascadeRouter> ResolveCascade(
      std::string_view model,
      std::string* remapped_model = nullptr) const;

  [[nodiscard]] std::vector<std::string> GetRegisteredModels() const;
  [[nodiscard]] size_t RouteCount() const;

  /**
   * Alias cycle detection
   */
  static bool DetectCycle(
      const std::unordered_map<std::string, std::string>& aliases,
      std::string* cycle_desc = nullptr);

  /**
   * Factory constructing a ModelRouter populated with providers and defaults from ProviderRegistry
   */
  static std::shared_ptr<ModelRouter> CreateFromRegistry(
      const ProviderRegistry& registry,
      std::shared_ptr<DistributedRateLimiter> rate_limiter = nullptr);

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<DistributedRateLimiter> rate_limiter_;
  std::shared_ptr<CascadeRouter> default_cascade_;
  std::unordered_map<std::string, std::shared_ptr<CascadeRouter>> exact_routes_;
  std::unordered_map<std::string, std::shared_ptr<CascadeRouter>> wildcard_routes_; // prefix without "/*"
};

}  // namespace tessera
