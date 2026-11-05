#pragma once

#include "tessera/routing/circuit_types.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace tessera {

class IUpstreamClient {
 public:
  virtual ~IUpstreamClient() = default;

  /**
   * Executes an upstream chat completion request.
   * Formats payload, transmits over HTTP/2 or mock transport,
   * measures latency, and parses completion tokens.
   */
  [[nodiscard]] virtual UpstreamResponse Execute(const UpstreamRequest& request) = 0;

  /**
   * Unique identifier of the provider (e.g. "groq", "openrouter", "mock").
   */
  [[nodiscard]] virtual std::string_view ProviderName() const noexcept = 0;

  /**
   * Configured API base endpoint URL.
   */
  [[nodiscard]] virtual std::string_view Endpoint() const noexcept = 0;

  /**
   * Default model served by this client adapter.
   */
  [[nodiscard]] virtual std::string_view DefaultModel() const noexcept = 0;
};

class HttpUpstreamClient : public IUpstreamClient {
 public:
  explicit HttpUpstreamClient(UpstreamProviderConfig config);
  ~HttpUpstreamClient() override = default;

  [[nodiscard]] UpstreamResponse Execute(const UpstreamRequest& request) override;
  [[nodiscard]] std::string_view ProviderName() const noexcept override;
  [[nodiscard]] std::string_view Endpoint() const noexcept override;
  [[nodiscard]] std::string_view DefaultModel() const noexcept override;

  // JSON serialization & response extraction helpers
  static std::string SerializeOpenAIRequest(const UpstreamRequest& req);
  static UpstreamResponse ParseOpenAIResponse(int status_code, std::string_view body, double latency_ms);

 private:
  UpstreamProviderConfig config_;
};

}  // namespace tessera

