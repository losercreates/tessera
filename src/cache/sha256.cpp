#include "tessera/cache/sha256.hpp"
#include <openssl/evp.h>
#include <iomanip>
#include <sstream>

namespace tessera {

std::string Sha256::Hash(std::string_view input) {
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int length = 0;

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) {
    return "";
  }

  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(ctx, input.data(), input.size()) != 1 ||
      EVP_DigestFinal_ex(ctx, hash, &length) != 1) {
    EVP_MD_CTX_free(ctx);
    return "";
  }

  EVP_MD_CTX_free(ctx);

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < length; ++i) {
    oss << std::setw(2) << static_cast<unsigned int>(hash[i]);
  }
  return oss.str();
}

std::string Sha256::ComputeCanonicalString(const ChatCompletionRequest& req) {
  std::ostringstream oss;
  oss << "model=" << req.model;
  if (req.temperature.has_value()) {
    oss << "|temp=" << std::fixed << std::setprecision(2) << req.temperature.value();
  }
  oss << "|messages=";
  for (size_t i = 0; i < req.messages.size(); ++i) {
    const auto& msg = req.messages[i];
    oss << "[" << i << ":role=" << msg.role << ":name=" << msg.name << ":content=" << msg.content << "]";
  }
  return oss.str();
}

std::string Sha256::GetExactCacheKey(const ChatCompletionRequest& req) {
  std::string canonical = ComputeCanonicalString(req);
  return "cache:exact:" + Hash(canonical);
}

}  // namespace tessera

