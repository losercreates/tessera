#pragma once

#include <string>

namespace tessera {

class CorrelationId {
 public:
  // Generate a random RFC 4122 version 4 UUID
  static std::string Generate();

  // Validate UUIDv4 format
  static bool IsValid(const std::string& id);
};

}  // namespace tessera

