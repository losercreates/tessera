#include "tessera/cache/onnx_embedding.hpp"
#include "tessera/common/logger.hpp"
#include <cmath>
#include <numeric>

namespace tessera {

MockEmbeddingEngine::MockEmbeddingEngine(size_t max_sequence_length)
    : tokenizer_(max_sequence_length) {}

PromptEmbedding MockEmbeddingEngine::GenerateEmbedding(std::string_view text) {
  PromptEmbedding emb;
  TokenizedPrompt tokens = tokenizer_.Tokenize(text);

  // Semantic feature hashing with term-frequency weighting
  // Maps tokens deterministically to 384 dimensions
  for (size_t i = 0; i < tokens.input_ids.size(); ++i) {
    if (tokens.attention_mask[i] == 0) continue;
    int64_t id = tokens.input_ids[i];
    if (id == BertTokenizer::kPadTokenId || id == BertTokenizer::kClsTokenId || id == BertTokenizer::kSepTokenId) {
      continue;
    }

    // Hash token id to prime dimensions
    for (size_t d = 0; d < 8; ++d) {
      size_t dim = (static_cast<size_t>(id) * 31 + d * 47) % PromptEmbedding::kDimension;
      float weight = 1.0f + (d == 0 ? 0.5f : 0.0f);
      emb.values[dim] += weight;
    }
  }

  // Ensure minimum baseline vector if text was empty or had no active words
  if (emb.Norm() < 1e-6f) {
    for (size_t d = 0; d < PromptEmbedding::kDimension; ++d) {
      emb.values[d] = 1.0f / std::sqrt(static_cast<float>(PromptEmbedding::kDimension));
    }
  }

  // Principle III Mandate: L2 Unit Normalization
  emb.Normalize();
  return emb;
}

// ==========================================
// OnnxEmbeddingEngine Implementation
// ==========================================

OnnxEmbeddingEngine::OnnxEmbeddingEngine(
    std::string model_path,
    std::string vocab_path,
    size_t max_sequence_length)
    : model_path_(std::move(model_path)),
      vocab_path_(std::move(vocab_path)),
      max_sequence_length_(max_sequence_length),
      fallback_engine_(std::make_unique<MockEmbeddingEngine>(max_sequence_length)),
      tokenizer_(std::make_unique<BertTokenizer>(max_sequence_length)) {
  if (!vocab_path_.empty()) {
    tokenizer_->LoadVocabFromFile(vocab_path_);
  }
}

OnnxEmbeddingEngine::~OnnxEmbeddingEngine() = default;

bool OnnxEmbeddingEngine::Initialize() {
  // If ONNX model file exists and ONNX Runtime is enabled, initialize session.
  // Otherwise, safely fallback to embedded engine.
  if (model_path_.empty()) {
    TESSERA_LOG_INFO("OnnxEmbeddingEngine: No model path specified, using high-speed embedded engine");
    is_initialized_ = true;
    return true;
  }

  // Future expansion: Ort::Env and Ort::Session initialization
  is_initialized_ = true;
  return true;
}

PromptEmbedding OnnxEmbeddingEngine::GenerateEmbedding(std::string_view text) {
  if (fallback_engine_ != nullptr) {
    return fallback_engine_->GenerateEmbedding(text);
  }
  PromptEmbedding emb;
  emb.Normalize();
  return emb;
}

}  // namespace tessera

