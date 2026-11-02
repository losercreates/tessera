#include "tessera/cache/exact_cache.hpp"
#include "tessera/cache/sha256.hpp"
#include "tessera/common/logger.hpp"
#include <sstream>
#include <iomanip>

namespace tessera {

// Helper to escape JSON strings
static std::string JsonEscape(std::string_view s) {
  std::ostringstream o;
  for (char c : s) {
    switch (c) {
      case '"': o << "\\\""; break;
      case '\\': o << "\\\\"; break;
      case '\b': o << "\\b"; break;
      case '\f': o << "\\f"; break;
      case '\n': o << "\\n"; break;
      case '\r': o << "\\r"; break;
      case '\t': o << "\\t"; break;
      default:
        if ('\x00' <= c && c <= '\x1f') {
          o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
        } else {
          o << c;
        }
    }
  }
  return o.str();
}

ExactCacheService::ExactCacheService(
    std::shared_ptr<RedisConnectionPool> redis_pool,
    uint32_t default_ttl_seconds)
    : redis_pool_(std::move(redis_pool)),
      default_ttl_seconds_(default_ttl_seconds) {}

std::string ExactCacheService::SerializeResponse(const ChatCompletionResponse& resp) {
  std::ostringstream oss;
  oss << "{"
      << "\"id\":\"" << JsonEscape(resp.id) << "\","
      << "\"created\":" << resp.created << ","
      << "\"model\":\"" << JsonEscape(resp.model) << "\","
      << "\"correlation_id\":\"" << JsonEscape(resp.correlation_id) << "\","
      << "\"choices\":[";

  for (size_t i = 0; i < resp.choices.size(); ++i) {
    if (i > 0) oss << ",";
    const auto& ch = resp.choices[i];
    oss << "{"
        << "\"index\":" << ch.index << ","
        << "\"role\":\"" << JsonEscape(ch.message.role) << "\","
        << "\"content\":\"" << JsonEscape(ch.message.content) << "\","
        << "\"name\":\"" << JsonEscape(ch.message.name) << "\","
        << "\"finish_reason\":\"" << JsonEscape(ch.finish_reason) << "\""
        << "}";
  }

  oss << "],"
      << "\"usage\":{"
      << "\"prompt_tokens\":" << resp.usage.prompt_tokens << ","
      << "\"completion_tokens\":" << resp.usage.completion_tokens << ","
      << "\"total_tokens\":" << resp.usage.total_tokens
      << "}"
      << "}";
  return oss.str();
}

std::optional<ChatCompletionResponse> ExactCacheService::DeserializeResponse(std::string_view json_str) {
  if (json_str.empty()) return std::nullopt;

  // Lightweight robust JSON extractor for ChatCompletionResponse
  auto find_string_val = [](std::string_view json, std::string_view key) -> std::string {
    std::string needle = "\"" + std::string(key) + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string_view::npos) return "";
    pos += needle.size();
    size_t end = json.find('"', pos);
    if (end == std::string_view::npos) return "";
    return std::string(json.substr(pos, end - pos));
  };

  auto find_int_val = [](std::string_view json, std::string_view key) -> int64_t {
    std::string needle = "\"" + std::string(key) + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string_view::npos) return 0;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    size_t end = pos;
    while (end < json.size() && (std::isdigit(json[end]) || json[end] == '-')) end++;
    if (end == pos) return 0;
    return std::stoll(std::string(json.substr(pos, end - pos)));
  };

  ChatCompletionResponse resp;
  resp.id = find_string_val(json_str, "id");
  resp.created = find_int_val(json_str, "created");
  resp.model = find_string_val(json_str, "model");
  resp.correlation_id = find_string_val(json_str, "correlation_id");

  // Extract message and choices
  Choice ch;
  ch.index = static_cast<uint32_t>(find_int_val(json_str, "index"));
  ch.message.role = find_string_val(json_str, "role");
  if (ch.message.role.empty()) ch.message.role = "assistant";
  ch.message.content = find_string_val(json_str, "content");
  ch.message.name = find_string_val(json_str, "name");
  ch.finish_reason = find_string_val(json_str, "finish_reason");
  if (ch.finish_reason.empty()) ch.finish_reason = "stop";
  resp.choices.push_back(std::move(ch));

  // Extract usage
  resp.usage.prompt_tokens = static_cast<uint32_t>(find_int_val(json_str, "prompt_tokens"));
  resp.usage.completion_tokens = static_cast<uint32_t>(find_int_val(json_str, "completion_tokens"));
  resp.usage.total_tokens = static_cast<uint32_t>(find_int_val(json_str, "total_tokens"));
  if (resp.usage.total_tokens == 0) {
    resp.usage.total_tokens = resp.usage.prompt_tokens + resp.usage.completion_tokens;
  }

  return resp;
}

CacheLookupResult ExactCacheService::Lookup(const ChatCompletionRequest& req) {
  auto start = std::chrono::steady_clock::now();
  CacheLookupResult result;

  if (redis_pool_ == nullptr || !redis_pool_->IsHealthy()) {
    return result;
  }

  std::string key = Sha256::GetExactCacheKey(req);
  PooledRedisConnection conn(*redis_pool_);
  if (!conn.IsValid()) {
    return result;
  }

  auto val = conn->Get(key);
  if (!val.has_value()) {
    auto end = std::chrono::steady_clock::now();
    result.latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
    return result;
  }

  auto resp_opt = DeserializeResponse(val.value());
  if (!resp_opt.has_value()) {
    return result;
  }

  auto end = std::chrono::steady_clock::now();
  result.hit = true;
  result.response = std::move(resp_opt.value());
  result.response.cache_hit = true;
  result.response.cache_tier = "exact";
  result.cache_tier = "exact";
  result.latency_ms = std::chrono::duration<double, std::milli>(end - start).count();

  TESSERA_LOG_DEBUG("Exact cache HIT for key {} in {:.3f}ms", key, result.latency_ms);
  return result;
}

bool ExactCacheService::Store(
    const ChatCompletionRequest& req,
    const ChatCompletionResponse& resp,
    uint32_t ttl_seconds) {
  if (redis_pool_ == nullptr) return false;

  uint32_t ttl = ttl_seconds > 0 ? ttl_seconds : default_ttl_seconds_;
  std::string key = Sha256::GetExactCacheKey(req);
  std::string val = SerializeResponse(resp);

  PooledRedisConnection conn(*redis_pool_);
  if (!conn.IsValid()) return false;

  bool ok = conn->Set(key, val, ttl);
  if (ok) {
    TESSERA_LOG_DEBUG("Stored exact cache entry for key {} (TTL: {}s)", key, ttl);
  }
  return ok;
}

}  // namespace tessera

