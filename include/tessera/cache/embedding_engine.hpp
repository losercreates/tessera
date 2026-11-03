#pragma once

#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>
#include <span>
#include <cstdint>
#include <cmath>

namespace tessera {

// Dense 384-dimensional normalized float embedding vector
struct PromptEmbedding {
  static constexpr size_t kDimension = 384;

  std::vector<float> values;

  PromptEmbedding();
  explicit PromptEmbedding(std::vector<float> vals);

  [[nodiscard]] size_t Dimension() const noexcept { return kDimension; }
  [[nodiscard]] bool IsNormalized() const noexcept;
  [[nodiscard]] float Norm() const noexcept;
  void Normalize();

  // RediSearch 1,536-byte IEEE 754 float32 contiguous binary representation
  [[nodiscard]] std::vector<uint8_t> ToBinaryBlob() const;
  static PromptEmbedding FromBinaryBlob(std::span<const uint8_t> blob);

  // Cosine similarity between two unit-normalized vectors: dot product
  static float CosineSimilarity(const PromptEmbedding& a, const PromptEmbedding& b);
  static float CosineDistance(const PromptEmbedding& a, const PromptEmbedding& b);
};

// Subword token representation for transformer input tensors
struct TokenizedPrompt {
  std::vector<int64_t> input_ids;
  std::vector<int64_t> attention_mask;
  std::vector<int64_t> token_type_ids;
  size_t sequence_length{0};
};

// WordPiece BertTokenizer
class BertTokenizer {
 public:
  explicit BertTokenizer(size_t max_sequence_length = 128);

  bool LoadVocabFromFile(const std::string& vocab_path);
  void LoadDefaultVocab();

  TokenizedPrompt Tokenize(std::string_view text) const;

  [[nodiscard]] int64_t GetTokenId(std::string_view token) const;
  [[nodiscard]] size_t VocabSize() const noexcept { return vocab_.size(); }

  static constexpr int64_t kPadTokenId = 0;
  static constexpr int64_t kUnkTokenId = 6;
  static constexpr int64_t kClsTokenId = 7;
  static constexpr int64_t kSepTokenId = 8;
  static constexpr int64_t kMaskTokenId = 9;

 private:
  std::unordered_map<std::string, int64_t> vocab_;
  size_t max_sequence_length_{128};
};

// Base interface for text embedding engines
class IEmbeddingEngine {
 public:
  virtual ~IEmbeddingEngine() = default;

  virtual PromptEmbedding GenerateEmbedding(std::string_view text) = 0;

  [[nodiscard]] virtual size_t Dimension() const noexcept = 0;
  [[nodiscard]] virtual std::string_view ModelName() const noexcept = 0;
};

}  // namespace tessera

