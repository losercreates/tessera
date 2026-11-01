#pragma once

#include "tessera/common/types.hpp"
#include <memory>
#include <string_view>

namespace tessera {

class ICompletionBackend {
 public:
  virtual ~ICompletionBackend() = default;

  // Process synchronous or asynchronous completion
  virtual Status Complete(const ChatCompletionRequest& request, ChatCompletionResponse* response) = 0;

  // Human-readable backend identifier (e.g. "MockEcho", "RedisExactCache", "GroqPrimary")
  [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
};

}  // namespace tessera

