#include "tessera/common/json.hpp"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace tessera {

namespace {

void AppendEscapedString(std::string& out, std::string_view sv) {
  out.push_back('"');
  for (char ch : sv) {
    unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
          out.append(buf);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

void DumpValue(std::string& out, const JsonValue& val) {
  switch (val.Type()) {
    case JsonType::Null:
      out.append("null");
      break;
    case JsonType::Bool:
      out.append(val.AsBool() ? "true" : "false");
      break;
    case JsonType::Number: {
      double d = val.AsDouble();
      if (!std::isfinite(d)) {
        out.append("null");
      } else if (d == std::trunc(d) && d >= -9007199254740992.0 && d <= 9007199254740992.0) {
        out.append(std::to_string(static_cast<int64_t>(d)));
      } else {
        std::ostringstream oss;
        oss << d;
        out.append(oss.str());
      }
      break;
    }
    case JsonType::String:
      AppendEscapedString(out, val.AsString());
      break;
    case JsonType::Array: {
      out.push_back('[');
      const auto& arr = val.AsArray();
      for (size_t i = 0; i < arr.size(); ++i) {
        if (i > 0) out.push_back(',');
        DumpValue(out, arr[i]);
      }
      out.push_back(']');
      break;
    }
    case JsonType::Object: {
      out.push_back('{');
      const auto& obj = val.AsObject();
      for (size_t i = 0; i < obj.size(); ++i) {
        if (i > 0) out.push_back(',');
        AppendEscapedString(out, obj[i].first);
        out.push_back(':');
        DumpValue(out, obj[i].second);
      }
      out.push_back('}');
      break;
    }
  }
}

void AppendUtf8CodePoint(std::string& out, uint32_t cp) {
  if (cp <= 0x7Fu) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

class JsonParser {
 public:
  JsonParser(std::string_view input, size_t max_depth)
      : input_(input), max_depth_(max_depth) {}

  bool ParseRoot(JsonValue* out, std::string* error_msg) {
    SkipWhitespace();
    if (pos_ >= input_.size()) {
      return Fail("Empty JSON input", error_msg);
    }
    if (!ParseValue(out, 0, error_msg)) {
      return false;
    }
    SkipWhitespace();
    if (pos_ != input_.size()) {
      return Fail("Unexpected trailing characters in JSON", error_msg);
    }
    return true;
  }

 private:
  void SkipWhitespace() noexcept {
    while (pos_ < input_.size()) {
      char c = input_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool Fail(std::string msg, std::string* error_msg) const {
    if (error_msg) {
      *error_msg = std::move(msg);
    }
    return false;
  }

  bool ParseHex4(uint32_t* val) {
    if (pos_ + 4 > input_.size()) return false;
    uint32_t acc = 0;
    for (size_t i = 0; i < 4; ++i) {
      char c = input_[pos_++];
      acc <<= 4;
      if (c >= '0' && c <= '9') acc |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') acc |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') acc |= static_cast<uint32_t>(c - 'A' + 10);
      else return false;
    }
    *val = acc;
    return true;
  }

  bool ParseStringRaw(std::string* out, std::string* error_msg) {
    if (pos_ >= input_.size() || input_[pos_] != '"') {
      return Fail("Expected string opening quote", error_msg);
    }
    ++pos_;
    std::string result;
    while (pos_ < input_.size()) {
      unsigned char uc = static_cast<unsigned char>(input_[pos_++]);
      if (uc == '"') {
        *out = std::move(result);
        return true;
      }
      if (uc < 0x20) {
        return Fail("Unescaped control character in JSON string", error_msg);
      }
      if (uc == '\\') {
        if (pos_ >= input_.size()) {
          return Fail("Unterminated escape sequence in JSON string", error_msg);
        }
        char esc = input_[pos_++];
        switch (esc) {
          case '"': result.push_back('"'); break;
          case '\\': result.push_back('\\'); break;
          case '/': result.push_back('/'); break;
          case 'b': result.push_back('\b'); break;
          case 'f': result.push_back('\f'); break;
          case 'n': result.push_back('\n'); break;
          case 'r': result.push_back('\r'); break;
          case 't': result.push_back('\t'); break;
          case 'u': {
            uint32_t cp = 0;
            if (!ParseHex4(&cp)) {
              return Fail("Invalid \\uXXXX escape in JSON string", error_msg);
            }
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
              if (pos_ + 2 <= input_.size() && input_[pos_] == '\\' && input_[pos_ + 1] == 'u') {
                pos_ += 2;
                uint32_t low = 0;
                if (!ParseHex4(&low) || low < 0xDC00u || low > 0xDFFFu) {
                  return Fail("Invalid UTF-16 surrogate pair in JSON string", error_msg);
                }
                uint32_t full = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                AppendUtf8CodePoint(result, full);
              } else {
                return Fail("Unpaired high surrogate in JSON string", error_msg);
              }
            } else {
              AppendUtf8CodePoint(result, cp);
            }
            break;
          }
          default:
            return Fail("Unsupported escape character in JSON string", error_msg);
        }
      } else {
        result.push_back(static_cast<char>(uc));
      }
    }
    return Fail("Unterminated JSON string", error_msg);
  }

  bool ParseNumber(JsonValue* out, std::string* error_msg) {
    size_t start = pos_;
    if (input_[pos_] == '-') ++pos_;
    if (pos_ >= input_.size()) {
      return Fail("Invalid JSON number", error_msg);
    }
    if (input_[pos_] == '0') {
      ++pos_;
    } else if (input_[pos_] >= '1' && input_[pos_] <= '9') {
      while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        ++pos_;
      }
    } else {
      return Fail("Invalid JSON number prefix", error_msg);
    }
    if (pos_ < input_.size() && input_[pos_] == '.') {
      ++pos_;
      if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        return Fail("Invalid decimal fraction in JSON number", error_msg);
      }
      while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        ++pos_;
      }
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
      if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        return Fail("Invalid exponent in JSON number", error_msg);
      }
      while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
        ++pos_;
      }
    }

    std::string token(input_.substr(start, pos_ - start));
    char* end_ptr = nullptr;
    double d = std::strtod(token.c_str(), &end_ptr);
    if (end_ptr != token.c_str() + token.size() || !std::isfinite(d)) {
      return Fail("Number out of range in JSON", error_msg);
    }
    *out = JsonValue(d);
    return true;
  }

  bool ParseValue(JsonValue* out, size_t depth, std::string* error_msg) {
    if (depth > max_depth_) {
      return Fail("JSON nesting depth exceeded maximum limit", error_msg);
    }
    SkipWhitespace();
    if (pos_ >= input_.size()) {
      return Fail("Unexpected end of JSON input", error_msg);
    }

    char c = input_[pos_];
    if (c == '"') {
      std::string str;
      if (!ParseStringRaw(&str, error_msg)) return false;
      *out = JsonValue(std::move(str));
      return true;
    }
    if (c == '{') {
      ++pos_;
      JsonValue::Object obj;
      SkipWhitespace();
      if (pos_ < input_.size() && input_[pos_] == '}') {
        ++pos_;
        *out = JsonValue(std::move(obj));
        return true;
      }
      while (true) {
        SkipWhitespace();
        std::string key;
        if (!ParseStringRaw(&key, error_msg)) return false;
        SkipWhitespace();
        if (pos_ >= input_.size() || input_[pos_] != ':') {
          return Fail("Expected ':' after object key", error_msg);
        }
        ++pos_;
        JsonValue val;
        if (!ParseValue(&val, depth + 1, error_msg)) return false;

        bool replaced = false;
        for (auto& kv : obj) {
          if (kv.first == key) {
            kv.second = std::move(val);
            replaced = true;
            break;
          }
        }
        if (!replaced) {
          obj.emplace_back(std::move(key), std::move(val));
        }

        SkipWhitespace();
        if (pos_ >= input_.size()) {
          return Fail("Unterminated JSON object", error_msg);
        }
        if (input_[pos_] == '}') {
          ++pos_;
          *out = JsonValue(std::move(obj));
          return true;
        }
        if (input_[pos_] != ',') {
          return Fail("Expected ',' or '}' in JSON object", error_msg);
        }
        ++pos_;
      }
    }
    if (c == '[') {
      ++pos_;
      JsonValue::Array arr;
      SkipWhitespace();
      if (pos_ < input_.size() && input_[pos_] == ']') {
        ++pos_;
        *out = JsonValue(std::move(arr));
        return true;
      }
      while (true) {
        JsonValue elem;
        if (!ParseValue(&elem, depth + 1, error_msg)) return false;
        arr.push_back(std::move(elem));
        SkipWhitespace();
        if (pos_ >= input_.size()) {
          return Fail("Unterminated JSON array", error_msg);
        }
        if (input_[pos_] == ']') {
          ++pos_;
          *out = JsonValue(std::move(arr));
          return true;
        }
        if (input_[pos_] != ',') {
          return Fail("Expected ',' or ']' in JSON array", error_msg);
        }
        ++pos_;
      }
    }
    if (input_.substr(pos_, 4) == "true") {
      pos_ += 4;
      *out = JsonValue(true);
      return true;
    }
    if (input_.substr(pos_, 5) == "false") {
      pos_ += 5;
      *out = JsonValue(false);
      return true;
    }
    if (input_.substr(pos_, 4) == "null") {
      pos_ += 4;
      *out = JsonValue(nullptr);
      return true;
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      return ParseNumber(out, error_msg);
    }
    return Fail("Unexpected token in JSON", error_msg);
  }

  std::string_view input_;
  size_t max_depth_;
  size_t pos_{0};
};

}  // namespace

JsonType JsonValue::Type() const noexcept {
  if (IsNull()) return JsonType::Null;
  if (IsBool()) return JsonType::Bool;
  if (IsNumber()) return JsonType::Number;
  if (IsString()) return JsonType::String;
  if (IsArray()) return JsonType::Array;
  return JsonType::Object;
}

bool JsonValue::AsBool(bool default_val) const noexcept {
  if (const auto* p = std::get_if<bool>(&data_)) return *p;
  return default_val;
}

int64_t JsonValue::AsInt64(int64_t default_val) const noexcept {
  if (const auto* p = std::get_if<double>(&data_)) return static_cast<int64_t>(*p);
  return default_val;
}

double JsonValue::AsDouble(double default_val) const noexcept {
  if (const auto* p = std::get_if<double>(&data_)) return *p;
  return default_val;
}

const std::string& JsonValue::AsString() const noexcept {
  static const std::string kEmpty;
  if (const auto* p = std::get_if<std::string>(&data_)) return *p;
  return kEmpty;
}

const JsonValue::Array& JsonValue::AsArray() const noexcept {
  static const Array kEmpty;
  if (const auto* p = std::get_if<Array>(&data_)) return *p;
  return kEmpty;
}

const JsonValue::Object& JsonValue::AsObject() const noexcept {
  static const Object kEmpty;
  if (const auto* p = std::get_if<Object>(&data_)) return *p;
  return kEmpty;
}

void JsonValue::Set(std::string key, JsonValue val) {
  auto* obj = std::get_if<Object>(&data_);
  if (obj == nullptr) {
    data_ = Object{};
    obj = std::get_if<Object>(&data_);
  }
  for (auto& kv : *obj) {
    if (kv.first == key) {
      kv.second = std::move(val);
      return;
    }
  }
  obj->emplace_back(std::move(key), std::move(val));
}

void JsonValue::PushBack(JsonValue val) {
  auto* arr = std::get_if<Array>(&data_);
  if (arr == nullptr) {
    data_ = Array{};
    arr = std::get_if<Array>(&data_);
  }
  arr->push_back(std::move(val));
}

const JsonValue* JsonValue::Find(std::string_view key) const noexcept {
  const auto* obj = std::get_if<Object>(&data_);
  if (obj == nullptr) return nullptr;
  for (const auto& kv : *obj) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

std::string JsonValue::Dump() const {
  std::string out;
  DumpValue(out, *this);
  return out;
}

bool JsonValue::Parse(
    std::string_view input,
    JsonValue* out,
    std::string* error_msg,
    size_t max_depth) {
  if (out == nullptr) {
    if (error_msg) *error_msg = "Output JsonValue pointer cannot be null";
    return false;
  }
  JsonParser parser(input, max_depth);
  return parser.ParseRoot(out, error_msg);
}

namespace json_codec {

Status ParseOpenAIChatRequest(std::string_view body, ChatCompletionRequest* req) {
  if (req == nullptr) {
    return Status::InvalidArgument("Request pointer cannot be null");
  }
  JsonValue root;
  std::string parse_err;
  if (!JsonValue::Parse(body, &root, &parse_err)) {
    return Status::InvalidArgument("Invalid JSON body: " + parse_err);
  }
  std::string err;
  if (!JsonToChatRequest(root, req, &err)) {
    return Status::InvalidArgument(err);
  }
  return Status::OK();
}

std::string SerializeOpenAIChatResponse(const ChatCompletionResponse& resp) {
  JsonValue out = JsonValue::MakeObject();
  out.Set("id", resp.id.empty() ? "chatcmpl-tessera" : resp.id);
  out.Set("object", "chat.completion");
  out.Set("created", resp.created);
  out.Set("model", resp.model);

  JsonValue choices = JsonValue::MakeArray();
  for (const auto& c : resp.choices) {
    JsonValue cj = JsonValue::MakeObject();
    cj.Set("index", c.index);
    JsonValue mj = JsonValue::MakeObject();
    mj.Set("role", c.message.role.empty() ? "assistant" : c.message.role);
    mj.Set("content", c.message.content);
    if (!c.message.name.empty()) mj.Set("name", c.message.name);
    cj.Set("message", std::move(mj));
    cj.Set("finish_reason", c.finish_reason.empty() ? "stop" : c.finish_reason);
    choices.PushBack(std::move(cj));
  }
  out.Set("choices", std::move(choices));

  JsonValue usage = JsonValue::MakeObject();
  usage.Set("prompt_tokens", resp.usage.prompt_tokens);
  usage.Set("completion_tokens", resp.usage.completion_tokens);
  usage.Set("total_tokens", resp.usage.total_tokens);
  out.Set("usage", std::move(usage));

  if (!resp.correlation_id.empty()) {
    out.Set("correlation_id", resp.correlation_id);
  }
  return out.Dump();
}

std::string SerializeOpenAISseChunk(
    const ChatCompletionResponse& resp,
    std::string_view role,
    std::string_view content_delta,
    std::string_view finish_reason) {
  JsonValue chunk = JsonValue::MakeObject();
  chunk.Set("id", resp.id.empty() ? "chatcmpl-tessera" : resp.id);
  chunk.Set("object", "chat.completion.chunk");
  chunk.Set("created", resp.created);
  chunk.Set("model", resp.model);

  JsonValue delta = JsonValue::MakeObject();
  if (!role.empty()) {
    delta.Set("role", std::string(role));
  }
  if (!content_delta.empty() || !role.empty()) {
    delta.Set("content", std::string(content_delta));
  }

  JsonValue choice = JsonValue::MakeObject();
  choice.Set("index", static_cast<uint32_t>(0));
  choice.Set("delta", std::move(delta));
  if (finish_reason.empty()) {
    choice.Set("finish_reason", JsonValue(nullptr));
  } else {
    choice.Set("finish_reason", std::string(finish_reason));
  }

  JsonValue choices = JsonValue::MakeArray();
  choices.PushBack(std::move(choice));
  chunk.Set("choices", std::move(choices));

  return chunk.Dump();
}

std::string SerializeOpenAIError(
    std::string_view message,
    std::string_view type,
    std::string_view code) {
  JsonValue err = JsonValue::MakeObject();
  err.Set("message", std::string(message));
  err.Set("type", std::string(type));
  err.Set("param", JsonValue(nullptr));
  if (code.empty()) {
    err.Set("code", JsonValue(nullptr));
  } else {
    err.Set("code", std::string(code));
  }

  JsonValue wrapper = JsonValue::MakeObject();
  wrapper.Set("error", std::move(err));
  return wrapper.Dump();
}

std::string SerializeOpenAIModels(const std::vector<ListedModel>& models) {
  JsonValue root = JsonValue::MakeObject();
  root.Set("object", "list");
  JsonValue data = JsonValue::MakeArray();
  for (const auto& m : models) {
    JsonValue item = JsonValue::MakeObject();
    item.Set("id", m.id);
    item.Set("object", "model");
    item.Set("created", m.created);
    item.Set("owned_by", m.owned_by.empty() ? "tessera" : m.owned_by);
    data.PushBack(std::move(item));
  }
  root.Set("data", std::move(data));
  return root.Dump();
}

JsonValue ChatRequestToJson(const ChatCompletionRequest& req) {
  JsonValue obj = JsonValue::MakeObject();
  obj.Set("model", req.model);
  JsonValue msgs = JsonValue::MakeArray();
  for (const auto& m : req.messages) {
    JsonValue mj = JsonValue::MakeObject();
    mj.Set("role", m.role);
    mj.Set("content", m.content);
    if (!m.name.empty()) {
      mj.Set("name", m.name);
    }
    msgs.PushBack(std::move(mj));
  }
  obj.Set("messages", std::move(msgs));
  if (req.temperature.has_value()) {
    obj.Set("temperature", static_cast<double>(*req.temperature));
  }
  if (req.max_tokens.has_value()) {
    obj.Set("max_tokens", *req.max_tokens);
  }
  obj.Set("stream", req.stream);
  if (!req.correlation_id.empty()) {
    obj.Set("correlation_id", req.correlation_id);
  }
  obj.Set("no_cache", req.no_cache);
  return obj;
}

bool JsonToChatRequest(
    const JsonValue& root,
    ChatCompletionRequest* req,
    std::string* error_msg) {
  if (req == nullptr || !root.IsObject()) {
    if (error_msg) *error_msg = "JSON root must be an object";
    return false;
  }

  *req = ChatCompletionRequest{};

  if (const JsonValue* model = root.Find("model")) {
    if (!model->IsString()) {
      if (error_msg) *error_msg = "Field 'model' must be a string";
      return false;
    }
    req->model = model->AsString();
  }

  if (const JsonValue* msgs = root.Find("messages")) {
    if (!msgs->IsArray()) {
      if (error_msg) *error_msg = "Field 'messages' must be an array";
      return false;
    }
    for (const auto& item : msgs->AsArray()) {
      if (!item.IsObject()) {
        if (error_msg) *error_msg = "Each entry in 'messages' must be an object";
        return false;
      }
      ChatMessage msg;
      if (const JsonValue* role = item.Find("role")) {
        if (!role->IsString()) {
          if (error_msg) *error_msg = "Message 'role' must be a string";
          return false;
        }
        msg.role = role->AsString();
      }
      if (const JsonValue* content = item.Find("content")) {
        if (!content->IsString()) {
          if (error_msg) *error_msg = "Message 'content' must be a string";
          return false;
        }
        msg.content = content->AsString();
      }
      if (const JsonValue* name = item.Find("name")) {
        if (!name->IsString() && !name->IsNull()) {
          if (error_msg) *error_msg = "Message 'name' must be a string";
          return false;
        }
        if (name->IsString()) msg.name = name->AsString();
      }
      req->messages.push_back(std::move(msg));
    }
  }

  if (const JsonValue* temp = root.Find("temperature")) {
    if (!temp->IsNull()) {
      if (!temp->IsNumber()) {
        if (error_msg) *error_msg = "Field 'temperature' must be a number";
        return false;
      }
      req->temperature = static_cast<float>(temp->AsDouble());
    }
  }

  if (const JsonValue* max_tok = root.Find("max_tokens")) {
    if (!max_tok->IsNull()) {
      if (!max_tok->IsNumber() || max_tok->AsDouble() < 0.0 ||
          max_tok->AsDouble() > static_cast<double>(UINT32_MAX)) {
        if (error_msg) *error_msg = "Field 'max_tokens' must be a non-negative integer";
        return false;
      }
      req->max_tokens = static_cast<uint32_t>(max_tok->AsInt64());
    }
  }

  if (const JsonValue* stream = root.Find("stream")) {
    if (!stream->IsBool()) {
      if (error_msg) *error_msg = "Field 'stream' must be a boolean";
      return false;
    }
    req->stream = stream->AsBool();
  }

  if (const JsonValue* no_cache = root.Find("no_cache")) {
    if (!no_cache->IsBool()) {
      if (error_msg) *error_msg = "Field 'no_cache' must be a boolean";
      return false;
    }
    req->no_cache = no_cache->AsBool();
  }

  if (const JsonValue* cid = root.Find("correlation_id")) {
    if (!cid->IsNull()) {
      if (!cid->IsString()) {
        if (error_msg) *error_msg = "Field 'correlation_id' must be a string";
        return false;
      }
      req->correlation_id = cid->AsString();
    }
  }

  return true;
}

JsonValue ChatResponseToJson(const ChatCompletionResponse& resp) {
  JsonValue obj = JsonValue::MakeObject();
  obj.Set("id", resp.id);
  obj.Set("created", resp.created);
  obj.Set("model", resp.model);

  JsonValue choices = JsonValue::MakeArray();
  for (const auto& c : resp.choices) {
    JsonValue cj = JsonValue::MakeObject();
    cj.Set("index", c.index);
    JsonValue mj = JsonValue::MakeObject();
    mj.Set("role", c.message.role);
    mj.Set("content", c.message.content);
    if (!c.message.name.empty()) mj.Set("name", c.message.name);
    cj.Set("message", std::move(mj));
    cj.Set("finish_reason", c.finish_reason);
    choices.PushBack(std::move(cj));
  }
  obj.Set("choices", std::move(choices));

  JsonValue usage = JsonValue::MakeObject();
  usage.Set("prompt_tokens", resp.usage.prompt_tokens);
  usage.Set("completion_tokens", resp.usage.completion_tokens);
  usage.Set("total_tokens", resp.usage.total_tokens);
  obj.Set("usage", std::move(usage));

  obj.Set("correlation_id", resp.correlation_id);
  obj.Set("cache_hit", resp.cache_hit);
  obj.Set("cache_tier", resp.cache_tier);
  return obj;
}

bool JsonToChatResponse(
    const JsonValue& root,
    ChatCompletionResponse* resp,
    std::string* error_msg) {
  if (resp == nullptr || !root.IsObject()) {
    if (error_msg) *error_msg = "JSON response root must be an object";
    return false;
  }

  *resp = ChatCompletionResponse{};
  if (const JsonValue* v = root.Find("id"); v && v->IsString()) resp->id = v->AsString();
  if (const JsonValue* v = root.Find("created"); v && v->IsNumber()) resp->created = v->AsInt64();
  if (const JsonValue* v = root.Find("model"); v && v->IsString()) resp->model = v->AsString();
  if (const JsonValue* v = root.Find("correlation_id"); v && v->IsString()) {
    resp->correlation_id = v->AsString();
  }
  if (const JsonValue* v = root.Find("cache_hit"); v && v->IsBool()) resp->cache_hit = v->AsBool();
  if (const JsonValue* v = root.Find("cache_tier"); v && v->IsString()) resp->cache_tier = v->AsString();

  if (const JsonValue* choices = root.Find("choices"); choices && choices->IsArray()) {
    for (const auto& c : choices->AsArray()) {
      if (!c.IsObject()) continue;
      Choice ch;
      if (const JsonValue* idx = c.Find("index"); idx && idx->IsNumber()) {
        ch.index = static_cast<uint32_t>(idx->AsInt64());
      }
      if (const JsonValue* fr = c.Find("finish_reason"); fr && fr->IsString()) {
        ch.finish_reason = fr->AsString();
      }
      if (const JsonValue* m = c.Find("message"); m && m->IsObject()) {
        if (const JsonValue* r = m->Find("role"); r && r->IsString()) ch.message.role = r->AsString();
        if (const JsonValue* cnt = m->Find("content"); cnt && cnt->IsString()) {
          ch.message.content = cnt->AsString();
        }
        if (const JsonValue* n = m->Find("name"); n && n->IsString()) ch.message.name = n->AsString();
      }
      resp->choices.push_back(std::move(ch));
    }
  }

  if (const JsonValue* u = root.Find("usage"); u && u->IsObject()) {
    if (const JsonValue* pt = u->Find("prompt_tokens"); pt && pt->IsNumber()) {
      resp->usage.prompt_tokens = static_cast<uint32_t>(pt->AsInt64());
    }
    if (const JsonValue* ct = u->Find("completion_tokens"); ct && ct->IsNumber()) {
      resp->usage.completion_tokens = static_cast<uint32_t>(ct->AsInt64());
    }
    if (const JsonValue* tt = u->Find("total_tokens"); tt && tt->IsNumber()) {
      resp->usage.total_tokens = static_cast<uint32_t>(tt->AsInt64());
    }
  }

  return true;
}

}  // namespace json_codec

}  // namespace tessera
