#include "tessera/cache/embedding_engine.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace tessera {

PromptEmbedding::PromptEmbedding() : values(kDimension, 0.0f) {}

PromptEmbedding::PromptEmbedding(std::vector<float> vals) : values(std::move(vals)) {
  if (values.size() != kDimension) {
    values.resize(kDimension, 0.0f);
  }
}

float PromptEmbedding::Norm() const noexcept {
  double sum = 0.0;
  for (float v : values) {
    sum += static_cast<double>(v) * static_cast<double>(v);
  }
  return static_cast<float>(std::sqrt(sum));
}

bool PromptEmbedding::IsNormalized() const noexcept {
  float n = Norm();
  return std::abs(n - 1.0f) < 1e-4f;
}

void PromptEmbedding::Normalize() {
  float n = Norm();
  if (n > 1e-12f) {
    float inv = 1.0f / n;
    for (float& v : values) {
      v *= inv;
    }
  } else if (!values.empty()) {
    values[0] = 1.0f;
  }
}

std::vector<uint8_t> PromptEmbedding::ToBinaryBlob() const {
  std::vector<uint8_t> blob(kDimension * sizeof(float));
  std::memcpy(blob.data(), values.data(), blob.size());
  return blob;
}

PromptEmbedding PromptEmbedding::FromBinaryBlob(std::span<const uint8_t> blob) {
  PromptEmbedding emb;
  if (blob.size() >= kDimension * sizeof(float)) {
    std::memcpy(emb.values.data(), blob.data(), kDimension * sizeof(float));
  }
  return emb;
}

float PromptEmbedding::CosineSimilarity(const PromptEmbedding& a, const PromptEmbedding& b) {
  double dot = 0.0;
  size_t n = std::min(a.values.size(), b.values.size());
  for (size_t i = 0; i < n; ++i) {
    dot += static_cast<double>(a.values[i]) * static_cast<double>(b.values[i]);
  }
  return std::clamp(static_cast<float>(dot), -1.0f, 1.0f);
}

float PromptEmbedding::CosineDistance(const PromptEmbedding& a, const PromptEmbedding& b) {
  return std::max(0.0f, 1.0f - CosineSimilarity(a, b));
}

// ==========================================
// BertTokenizer Implementation
// ==========================================

BertTokenizer::BertTokenizer(size_t max_sequence_length)
    : max_sequence_length_(max_sequence_length) {
  LoadDefaultVocab();
}

void BertTokenizer::LoadDefaultVocab() {
  vocab_.clear();
  std::vector<std::string> default_tokens = {
      "[PAD]", "[unused1]", "[unused2]", "[unused3]", "[unused4]", "[unused5]",
      "[UNK]", "[CLS]", "[SEP]", "[MASK]", "what", "is", "the", "capital", "of",
      "france", "paris", "tell", "me", "city", "french", "who", "how", "why",
      "where", "are", "a", "an", "to", "in", "for", "on", "with", "as", "by",
      "at", "from", "and", "or", "it", "its", "can", "you", "give", "name",
      "known", "explain", "about", "world", "country", "europe", "germany",
      "berlin", "spain", "madrid", "italy", "rome", "speed", "light", "quantum",
      "physics", "science", "computer", "programming", "gateway", "cache",
      "semantic", "exact", "redis", "vector", "embedding", "model", "llama",
      "gemma", "groq", "openrouter", "token", "rate", "limit", "query", "prompt",
      "response", "hello", "hi", "yes", "no", "true", "false", "test", "code",
      "function", "system", "user", "assistant", "##s", "##ing", "##ed", "##ly",
      "##er", "##al", "##ity", "##tion", "##able", "##ic", "##ize"
  };

  for (size_t i = 0; i < default_tokens.size(); ++i) {
    vocab_[default_tokens[i]] = static_cast<int64_t>(i);
  }
}

bool BertTokenizer::LoadVocabFromFile(const std::string& vocab_path) {
  std::ifstream infile(vocab_path);
  if (!infile.is_open()) {
    // Also try looking in parent directory (useful when running from build/)
    std::string fallback = "../" + vocab_path;
    infile.open(fallback);
    if (!infile.is_open()) {
      return false;
    }
  }

  vocab_.clear();
  std::string line;
  int64_t id = 0;
  while (std::getline(infile, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) {
      vocab_[line] = id++;
    }
  }
  return true;
}

int64_t BertTokenizer::GetTokenId(std::string_view token) const {
  auto it = vocab_.find(std::string(token));
  if (it != vocab_.end()) return it->second;
  return kUnkTokenId;
}

TokenizedPrompt BertTokenizer::Tokenize(std::string_view text) const {
  TokenizedPrompt prompt;
  prompt.sequence_length = max_sequence_length_;
  prompt.input_ids.reserve(max_sequence_length_);
  prompt.attention_mask.reserve(max_sequence_length_);
  prompt.token_type_ids.assign(max_sequence_length_, 0);

  // Always begin with [CLS]
  prompt.input_ids.push_back(kClsTokenId);
  prompt.attention_mask.push_back(1);

  // Normalize and split text into words
  std::string word;
  auto emit_word = [&](const std::string& w) {
    if (w.empty()) return;
    if (prompt.input_ids.size() >= max_sequence_length_ - 1) return; // leave room for [SEP]

    // Greedy longest match
    size_t start = 0;
    while (start < w.size() && prompt.input_ids.size() < max_sequence_length_ - 1) {
      size_t end = w.size();
      int64_t matched_id = -1;
      size_t matched_len = 0;

      while (end > start) {
        std::string sub = (start == 0) ? w.substr(start, end - start)
                                       : "##" + w.substr(start, end - start);
        auto it = vocab_.find(sub);
        if (it != vocab_.end()) {
          matched_id = it->second;
          matched_len = end - start;
          break;
        }
        end--;
      }

      if (matched_id >= 0) {
        prompt.input_ids.push_back(matched_id);
        prompt.attention_mask.push_back(1);
        start += matched_len;
      } else {
        prompt.input_ids.push_back(kUnkTokenId);
        prompt.attention_mask.push_back(1);
        break;
      }
    }
  };

  for (char c : text) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc)) {
      word.push_back(static_cast<char>(std::tolower(uc)));
    } else {
      emit_word(word);
      word.clear();
      if (!std::isspace(uc)) {
        std::string punct(1, static_cast<char>(c));
        emit_word(punct);
      }
    }
  }
  emit_word(word);

  // Append [SEP]
  prompt.input_ids.push_back(kSepTokenId);
  prompt.attention_mask.push_back(1);

  // Pad to max_sequence_length_
  while (prompt.input_ids.size() < max_sequence_length_) {
    prompt.input_ids.push_back(kPadTokenId);
    prompt.attention_mask.push_back(0);
  }

  return prompt;
}

}  // namespace tessera
