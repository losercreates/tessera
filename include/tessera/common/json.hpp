#pragma once

#include "tessera/common/types.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tessera {

enum class JsonType {
  Null,
  Bool,
  Number,
  String,
  Array,
  Object,
};

/**
 * Lightweight, self-contained JSON value representation supporting strict
 * depth-bounded recursive-descent parsing and escaped serialization.
 */
class JsonValue {
 public:
  using Array = std::vector<JsonValue>;
  using Object = std::vector<std::pair<std::string, JsonValue>>;

  JsonValue() : data_(nullptr) {}
  explicit(false) JsonValue(std::nullptr_t) : data_(nullptr) {}
  explicit(false) JsonValue(bool b) : data_(b) {}
  explicit(false) JsonValue(int n) : data_(static_cast<double>(n)) {}
  explicit(false) JsonValue(int64_t n) : data_(static_cast<double>(n)) {}
  explicit(false) JsonValue(uint32_t n) : data_(static_cast<double>(n)) {}
  explicit(false) JsonValue(uint64_t n) : data_(static_cast<double>(n)) {}
  explicit(false) JsonValue(double n) : data_(n) {}
  explicit(false) JsonValue(const char* s) : data_(s ? std::string(s) : std::string()) {}
  explicit(false) JsonValue(std::string s) : data_(std::move(s)) {}
  explicit(false) JsonValue(Array a) : data_(std::move(a)) {}
  explicit(false) JsonValue(Object o) : data_(std::move(o)) {}

  [[nodiscard]] static JsonValue MakeObject() { return JsonValue(Object{}); }
  [[nodiscard]] static JsonValue MakeArray() { return JsonValue(Array{}); }

  [[nodiscard]] JsonType Type() const noexcept;
  [[nodiscard]] bool IsNull() const noexcept { return std::holds_alternative<std::nullptr_t>(data_); }
  [[nodiscard]] bool IsBool() const noexcept { return std::holds_alternative<bool>(data_); }
  [[nodiscard]] bool IsNumber() const noexcept { return std::holds_alternative<double>(data_); }
  [[nodiscard]] bool IsString() const noexcept { return std::holds_alternative<std::string>(data_); }
  [[nodiscard]] bool IsArray() const noexcept { return std::holds_alternative<Array>(data_); }
  [[nodiscard]] bool IsObject() const noexcept { return std::holds_alternative<Object>(data_); }

  [[nodiscard]] bool AsBool(bool default_val = false) const noexcept;
  [[nodiscard]] int64_t AsInt64(int64_t default_val = 0) const noexcept;
  [[nodiscard]] double AsDouble(double default_val = 0.0) const noexcept;
  [[nodiscard]] const std::string& AsString() const noexcept;
  [[nodiscard]] const Array& AsArray() const noexcept;
  [[nodiscard]] const Object& AsObject() const noexcept;

  void Set(std::string key, JsonValue val);
  void PushBack(JsonValue val);
  [[nodiscard]] const JsonValue* Find(std::string_view key) const noexcept;

  [[nodiscard]] std::string Dump() const;

  static bool Parse(
      std::string_view input,
      JsonValue* out,
      std::string* error_msg = nullptr,
      size_t max_depth = 64);

 private:
  using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
  Storage data_;
};

namespace json_codec {

struct ListedModel {
  std::string id;
  std::string owned_by;
  int64_t created{0};
};

[[nodiscard]] Status ParseOpenAIChatRequest(std::string_view body, ChatCompletionRequest* req);
[[nodiscard]] std::string SerializeOpenAIChatResponse(const ChatCompletionResponse& resp);
[[nodiscard]] std::string SerializeOpenAISseChunk(
    const ChatCompletionResponse& resp,
    std::string_view role,
    std::string_view content_delta,
    std::string_view finish_reason);
[[nodiscard]] std::string SerializeOpenAIError(
    std::string_view message,
    std::string_view type,
    std::string_view code = "");
[[nodiscard]] std::string SerializeOpenAIModels(const std::vector<ListedModel>& models);

[[nodiscard]] JsonValue ChatRequestToJson(const ChatCompletionRequest& req);
[[nodiscard]] bool JsonToChatRequest(
    const JsonValue& root,
    ChatCompletionRequest* req,
    std::string* error_msg = nullptr);

[[nodiscard]] JsonValue ChatResponseToJson(const ChatCompletionResponse& resp);
[[nodiscard]] bool JsonToChatResponse(
    const JsonValue& root,
    ChatCompletionResponse* resp,
    std::string* error_msg = nullptr);

}  // namespace json_codec

}  // namespace tessera
