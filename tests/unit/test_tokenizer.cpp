#include <gtest/gtest.h>
#include "tessera/cache/embedding_engine.hpp"

using namespace tessera;

TEST(BertTokenizerTest, SpecialTokensPlacement) {
  BertTokenizer tokenizer(16);
  TokenizedPrompt tokens = tokenizer.Tokenize("what is the capital");

  ASSERT_GE(tokens.input_ids.size(), 4u);
  // [CLS] at position 0
  EXPECT_EQ(tokens.input_ids[0], BertTokenizer::kClsTokenId);
  EXPECT_EQ(tokens.attention_mask[0], 1);

  // Active word tokens in between
  EXPECT_GT(tokens.input_ids[1], 0);

  // [SEP] terminates active sequence
  size_t sep_pos = 0;
  for (size_t i = 1; i < tokens.input_ids.size(); ++i) {
    if (tokens.input_ids[i] == BertTokenizer::kSepTokenId) {
      sep_pos = i;
      break;
    }
  }
  EXPECT_GT(sep_pos, 0u);

  // Remaining tokens are [PAD] with attention_mask = 0
  for (size_t i = sep_pos + 1; i < tokens.input_ids.size(); ++i) {
    EXPECT_EQ(tokens.input_ids[i], BertTokenizer::kPadTokenId);
    EXPECT_EQ(tokens.attention_mask[i], 0);
  }
}

TEST(BertTokenizerTest, ContextLengthTruncation) {
  // Test truncation to fixed max length (e.g. 16)
  BertTokenizer tokenizer(16);
  std::string long_text = "what is the capital of france and why is paris known as the city of light and culture in europe world";
  TokenizedPrompt tokens = tokenizer.Tokenize(long_text);

  EXPECT_EQ(tokens.input_ids.size(), 16u);
  EXPECT_EQ(tokens.attention_mask.size(), 16u);
  EXPECT_EQ(tokens.input_ids[0], BertTokenizer::kClsTokenId);
  // The 16th token or last token before pad should be [SEP]
  EXPECT_EQ(tokens.input_ids.back(), BertTokenizer::kSepTokenId);
}

TEST(BertTokenizerTest, SubwordGreedySegmentation) {
  BertTokenizer tokenizer(32);
  // "querying" should split to "query" + "##ing"
  TokenizedPrompt tokens = tokenizer.Tokenize("querying");
  ASSERT_GE(tokens.input_ids.size(), 4u);
  EXPECT_EQ(tokens.input_ids[0], BertTokenizer::kClsTokenId);
  EXPECT_EQ(tokens.input_ids[1], tokenizer.GetTokenId("query"));
  EXPECT_EQ(tokens.input_ids[2], tokenizer.GetTokenId("##ing"));
  EXPECT_EQ(tokens.input_ids[3], BertTokenizer::kSepTokenId);
}

TEST(BertTokenizerTest, LoadCustomVocabFile) {
  BertTokenizer tokenizer(32);
  bool loaded = tokenizer.LoadVocabFromFile("tests/fixtures/vocab.txt");
  EXPECT_TRUE(loaded);
  EXPECT_GT(tokenizer.VocabSize(), 50u);
  EXPECT_EQ(tokenizer.GetTokenId("capital"), 13);
}

