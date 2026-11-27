#include <gtest/gtest.h>
#include "tessera/common/json.hpp"
#include <string>

using namespace tessera;

TEST(JsonTest, ParsesAndDumpsNestedObjectsAndArrays) {
  std::string raw = R"({
    "model": "gpt-4o",
    "temperature": 0.5,
    "max_tokens": 128,
    "stream": true,
    "no_cache": false,
    "meta": null,
    "messages": [
      {"role": "system", "content": "Be concise"},
      {"role": "user", "content": "Line1\nLine2 \"quoted\" \\slash \u0041"}
    ]
  })";

  JsonValue parsed;
  std::string err;
  ASSERT_TRUE(JsonValue::Parse(raw, &parsed, &err));
  EXPECT_TRUE(parsed.IsObject());
  EXPECT_EQ(parsed.Find("model")->AsString(), "gpt-4o");
  EXPECT_NEAR(parsed.Find("temperature")->AsDouble(), 0.5, 1e-6);
  EXPECT_EQ(parsed.Find("max_tokens")->AsInt64(), 128);
  EXPECT_TRUE(parsed.Find("stream")->AsBool());
  EXPECT_FALSE(parsed.Find("no_cache")->AsBool());
  EXPECT_TRUE(parsed.Find("meta")->IsNull());

  const JsonValue* msgs = parsed.Find("messages");
  ASSERT_NE(msgs, nullptr);
  ASSERT_TRUE(msgs->IsArray());
  ASSERT_EQ(msgs->AsArray().size(), 2u);
  EXPECT_EQ(msgs->AsArray()[1].Find("content")->AsString(), "Line1\nLine2 \"quoted\" \\slash A");

  // Re-parse dumped JSON cleanly
  std::string dumped = parsed.Dump();
  JsonValue reparsed;
  ASSERT_TRUE(JsonValue::Parse(dumped, &reparsed, &err));
  EXPECT_EQ(reparsed.Find("messages")->AsArray()[1].Find("content")->AsString(),
            "Line1\nLine2 \"quoted\" \\slash A");
}

TEST(JsonTest, ParsesUtf16SurrogatePairsToUtf8) {
  JsonValue val;
  std::string err;
  ASSERT_TRUE(JsonValue::Parse(R"({"emoji":"\uD83D\uDE00"})", &val, &err));
  EXPECT_EQ(val.Find("emoji")->AsString(), "\xF0\x9F\x98\x80");
}

TEST(JsonTest, RejectsMalformedTruncatedAndOverNestedInputs) {
  JsonValue val;
  std::string err;

  EXPECT_FALSE(JsonValue::Parse("", &val, &err));
  EXPECT_FALSE(JsonValue::Parse("{\"a\":", &val, &err));
  EXPECT_FALSE(JsonValue::Parse("{\"a\":1} trailing", &val, &err));
  EXPECT_FALSE(JsonValue::Parse("[1, 2,]", &val, &err));
  EXPECT_FALSE(JsonValue::Parse("\"unterminated", &val, &err));
  EXPECT_FALSE(JsonValue::Parse("{\"a\":\"\n\"}", &val, &err));  // unescaped control char
  EXPECT_FALSE(JsonValue::Parse("{\"a\":\"\\u12Z4\"}", &val, &err));

  // Exceeding depth limit fails safely without stack overflow
  std::string deep(100, '[');
  deep.append(100, ']');
  EXPECT_FALSE(JsonValue::Parse(deep, &val, &err, 32));
}

TEST(JsonTest, OpenAIChatRequestAndResponseRoundtrip) {
  std::string openai_req = R"({
    "model": "llama3.2",
    "messages": [
      {"role": "user", "content": "Explain caching"}
    ],
    "temperature": 0.2,
    "max_tokens": 256,
    "stream": true,
    "no_cache": true,
    "correlation_id": "cid-json-1"
  })";

  ChatCompletionRequest req;
  Status st = json_codec::ParseOpenAIChatRequest(openai_req, &req);
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(req.model, "llama3.2");
  ASSERT_EQ(req.messages.size(), 1u);
  EXPECT_EQ(req.messages[0].role, "user");
  EXPECT_EQ(req.messages[0].content, "Explain caching");
  ASSERT_TRUE(req.temperature.has_value());
  EXPECT_NEAR(*req.temperature, 0.2f, 1e-4f);
  ASSERT_TRUE(req.max_tokens.has_value());
  EXPECT_EQ(*req.max_tokens, 256u);
  EXPECT_TRUE(req.stream);
  EXPECT_TRUE(req.no_cache);
  EXPECT_EQ(req.correlation_id, "cid-json-1");

  // Type mismatch returns InvalidArgument
  ChatCompletionRequest bad_req;
  EXPECT_EQ(json_codec::ParseOpenAIChatRequest(R"({"model":123})", &bad_req).code,
            StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(json_codec::ParseOpenAIChatRequest(R"({"model":"x","messages":"bad"})", &bad_req).code,
            StatusCode::INVALID_ARGUMENT);

  // OpenAI Response serialization & SSE chunk serialization
  ChatCompletionResponse resp;
  resp.id = "cmpl-test";
  resp.created = 1700000000;
  resp.model = "llama3.2";
  resp.correlation_id = "cid-json-1";
  resp.cache_hit = true;
  resp.cache_tier = "exact";
  resp.choices.push_back(Choice{.index = 0, .message = ChatMessage{.role = "assistant", .content = "Hello!"}, .finish_reason = "stop"});
  resp.usage = Usage{.prompt_tokens = 10, .completion_tokens = 2, .total_tokens = 12};

  std::string openai_json = json_codec::SerializeOpenAIChatResponse(resp);
  JsonValue parsed_resp;
  ASSERT_TRUE(JsonValue::Parse(openai_json, &parsed_resp));
  EXPECT_EQ(parsed_resp.Find("object")->AsString(), "chat.completion");
  EXPECT_EQ(parsed_resp.Find("choices")->AsArray()[0].Find("message")->Find("content")->AsString(), "Hello!");

  std::string sse_chunk = json_codec::SerializeOpenAISseChunk(resp, "", "Hello!", "");
  JsonValue parsed_chunk;
  ASSERT_TRUE(JsonValue::Parse(sse_chunk, &parsed_chunk));
  EXPECT_EQ(parsed_chunk.Find("object")->AsString(), "chat.completion.chunk");
  EXPECT_EQ(parsed_chunk.Find("choices")->AsArray()[0].Find("delta")->Find("content")->AsString(), "Hello!");
}
