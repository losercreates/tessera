#pragma once

#include "tessera/backend/upstream_client.hpp"
#include "tessera/routing/circuit_breaker.hpp"
#include "tessera/routing/circuit_types.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tessera {

class DistributedRateLimiter;

using ClientFactory = std::function<std::shared_ptr<IUpstreamClient>(const ProviderConfig&)>;
using BreakerFactory = std::function<std::shared_ptr<ICircuitBreaker>(const CircuitBreakerConfig&)>;

class ProviderRegistry {
 public:
  ProviderRegistry();
  ~ProviderRegistry() = default;

  // Move-constructible & move-assignable
  ProviderRegistry(ProviderRegistry&&) noexcept;
  ProviderRegistry& operator=(ProviderRegistry&&) noexcept;

  // Non-copyable
  ProviderRegistry(const ProviderRegistry&) = delete;
  ProviderRegistry& operator=(const ProviderRegistry&) = delete;

  /**
   * Registers a new provider into the catalog.
   * Instantiates its dedicated IUpstreamClient and ICircuitBreaker.
   * Returns true on success, false if config is invalid or provider already exists.
   */
  bool RegisterProvider(const ProviderConfig& config, std::string* error_msg = nullptr);

  /**
   * Removes a provider from the catalog.
   */
  bool UnregisterProvider(std::string_view name);

  /**
   * Lookups
   */
  [[nodiscard]] std::shared_ptr<IUpstreamClient> GetProviderClient(std::string_view name) const;
  [[nodiscard]] std::shared_ptr<ICircuitBreaker> GetProviderBreaker(std::string_view name) const;
  [[nodiscard]] const ProviderConfig* GetProviderConfig(std::string_view name) const;

  [[nodiscard]] bool HasProvider(std::string_view name) const;
  [[nodiscard]] size_t ProviderCount() const;
  [[nodiscard]] std::vector<std::string> GetRegisteredProviders() const;

  /**
   * Optional Rate Limiter Integration
   */
  void SetRateLimiter(std::shared_ptr<DistributedRateLimiter> rate_limiter);
  void RegisterRateLimits(DistributedRateLimiter& limiter) const;

  /**
   * Dependency injection for mock testing
   */
  void SetClientFactory(ClientFactory factory);
  void SetBreakerFactory(BreakerFactory factory);

  /**
   * Configuration Loaders
   */
  static ProviderRegistry LoadFromEnvironment(
      bool preseed_defaults = true,
      ClientFactory custom_client_factory = nullptr);
  static ProviderRegistry LoadFromJson(std::string_view json_str);

  /**
   * Helper to generate default ProviderConfig for standard providers
   */
  static ProviderConfig DefaultGroqConfig();
  static ProviderConfig DefaultOpenRouterConfig();

 private:
  struct ProviderEntry {
    ProviderConfig config;
    std::shared_ptr<IUpstreamClient> client;
    std::shared_ptr<ICircuitBreaker> breaker;
  };

  mutable std::mutex mutex_;
  std::vector<std::string> provider_order_;
  std::unordered_map<std::string, ProviderEntry> providers_;
  ClientFactory client_factory_;
  BreakerFactory breaker_factory_;
  std::shared_ptr<DistributedRateLimiter> rate_limiter_;
};

}  // namespace tessera
