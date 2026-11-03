#pragma once

#include "tessera/cache/embedding_engine.hpp"
#include <string>
#include <memory>
#include <vector>

namespace tessera {

// High-speed, deterministic in-memory embedding engine for local & test execution
class MockEmbeddingEngine : public IEmbeddingEngine {
 public:
  explicit MockEmbeddingEngine(size_t max_sequence_length = 128);

  PromptEmbedding GenerateEmbedding(std::string_view text) override;

  [[nodiscard]] size_t Dimension() const noexcept override { return PromptEmbedding::kDimension; }
  [[nodiscard]] std::string_view ModelName() const noexcept override { return "mock-all-MiniLM-L6-v2"; }

  const BertTokenizer& GetTokenizer() const noexcept { return tokenizer_; }

 private:
  BertTokenizer tokenizer_;
};

// ONNX Runtime CPU inference runner for all-MiniLM-L6-v2
class OnnxEmbeddingEngine : public IEmbeddingEngine {
 public:
  explicit OnnxEmbeddingEngine(
      std::string model_path = "",
      std::string vocab_path = "",
      size_t max_sequence_length = 128);
  ~OnnxEmbeddingEngine() override;

  bool Initialize();
  [[nodiscard]] bool IsInitialized() const noexcept { return is_initialized_; }

  PromptEmbedding GenerateEmbedding(std::string_view text) override;

  [[nodiscard]] size_t Dimension() const noexcept override { return PromptEmbedding::kDimension; }
  [[nodiscard]] std::string_view ModelName() const noexcept override { return "all-MiniLM-L6-v2"; }

 private:
  std::string model_path_;
  std::string vocab_path_;
  size_t max_sequence_length_{128};
  bool is_initialized_{false};
  std::unique_ptr<MockEmbeddingEngine> fallback_engine_;
  std::unique_ptr<BertTokenizer> tokenizer_;
};

}  // namespace tessera

