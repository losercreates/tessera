#include "tessera/eval/evaluation_dataset.hpp"
#include <algorithm>
#include <array>
#include <string>

namespace tessera {

std::vector<EvaluationQuery> EvaluationDataset::GenerateDataset(
    size_t total_queries,
    double paraphrase_ratio) {
  size_t paraphrase_count = static_cast<size_t>(static_cast<double>(total_queries) * paraphrase_ratio);
  size_t base_count = total_queries - paraphrase_count;

  std::vector<EvaluationQuery> dataset;
  dataset.reserve(total_queries);

  // Seed base patterns across multiple domains
  struct QueryPair {
    std::string base;
    std::string paraphrase;
    std::string category;
  };

  const std::array<QueryPair, 30> seed_pairs = {{
      {"How do I reverse a singly linked list in C++?",
       "Write C++ code to invert a singly linked list.", "coding"},
      {"Explain the difference between TCP and UDP transport protocols.",
       "Compare the characteristics of TCP versus UDP networking.", "systems"},
      {"How does a Redis rate limit token bucket work?",
       "Explain how a Redis rate limit token bucket works.", "systems"},
      {"What is the time complexity of searching in a balanced binary search tree?",
       "What is the average and worst-case search time in an AVL or Red-Black tree?", "math"},
      {"Describe the two-phase commit protocol in distributed databases.",
       "How does 2PC ensure atomic distributed transactions?", "systems"},
      {"How do I detect a cycle in a directed graph using topological sort?",
       "Explain how to detect a cycle in a directed graph with topological sorting.", "coding"},
      {"How is vector cosine similarity calculated for dense embeddings?",
       "Explain how to calculate vector cosine similarity for dense embeddings.", "math"},
      {"What are the advantages of asynchronous I/O completion queues in gRPC C++?",
       "Why use async ServerCompletionQueue over synchronous threads in gRPC C++?", "systems"},
      {"How does the Raft consensus algorithm handle leader election?",
       "Explain the leader election phase in the Raft consensus protocol.", "systems"},
      {"What is the difference between mutex and spinlock in multithreaded programming?",
       "Compare mutexes and spinlocks in concurrent software design.", "coding"},
      {"Explain how WordPiece tokenization segments unseen vocabulary words.",
       "How does a subword WordPiece tokenizer break down out-of-vocabulary tokens?", "coding"},
      {"How does a 3-state circuit breaker protect downstream services from cascading failure?",
       "Describe CLOSED, OPEN, and HALF-OPEN transitions in a resilient circuit breaker.", "systems"},
      {"What is the purpose of random jitter in exponential backoff retry algorithms?",
       "Why do retry algorithms add random jitter in exponential backoff policies?", "math"},
      {"Explain how RediSearch vector indexes accelerate nearest neighbor search.",
       "How do RediSearch vector indexes accelerate nearest neighbor search?", "systems"},
      {"How does AddressSanitizer detect buffer overflows and use-after-free bugs?",
       "What mechanisms does ASan use to identify memory corruptions in C++ binaries?", "systems"},
      {"What is the difference between SHA-256 and MD5 cryptographic hash functions?",
       "Compare SHA-256 security properties against MD5 collision vulnerabilities.", "security"},
      {"How do modern CPU architectures handle branch prediction and speculative execution?",
       "Explain branch prediction mechanisms and speculative execution in modern processors.", "systems"},
      {"How does garbage collection in Go compare to RAII in Modern C++?",
       "Contrast automatic memory management in Go with deterministic RAII in C++.", "coding"},
      {"What is tail latency and why is p99 latency critical in microservice architectures?",
       "Explain tail latency and why p99 latency is critical in microservice architectures.", "systems"},
      {"How do zero-copy network buffers improve high-throughput server performance?",
       "Explain how zero-copy I/O eliminates kernel-to-user memory transfers.", "systems"},
      {"What is Amdahl Law and how does it calculate parallel speedup limits?",
       "Explain Amdahl Law and how to calculate parallel speedup limits.", "math"},
      {"How does an LRU cache eviction policy work using a hash map and doubly linked list?",
       "Describe the standard implementation of an O(1) Least Recently Used cache.", "coding"},
      {"What are memory barriers and acquire-release semantics in the C++ memory model?",
       "Explain std::memory_order_acquire and std::memory_order_release in lock-free C++.", "coding"},
      {"How do TLS 1.3 handshake protocols reduce network connection latency?",
       "Describe 1-RTT and 0-RTT handshakes in TLS 1.3 compared to TLS 1.2.", "security"},
      {"What is the difference between horizontal and vertical database sharding?",
       "Compare horizontal partitioning versus vertical partitioning in scalable database architectures.", "systems"},
      {"How does a bloom filter set membership data structure work?",
       "Explain how a bloom filter set membership data structure works.", "math"},
      {"What are coroutines in C++20 and how do they differ from OS threads?",
       "Explain co_await, co_yield, and stackless coroutines introduced in C++20.", "coding"},
      {"How do consistent hashing algorithms minimize key remaps during cache node scaling?",
       "Describe consistent hashing rings and virtual nodes in distributed caching.", "systems"},
      {"What is the purpose of HTTP/2 header compression using HPACK?",
       "How does HPACK reduce bandwidth consumption for repeated HTTP/2 request headers?", "systems"},
      {"Explain gradient descent optimization in machine learning model training.",
       "How do learning rate schedules affect convergence in stochastic gradient descent?", "math"}
  }};

  // 1. Generate base queries
  for (size_t i = 0; i < base_count; ++i) {
    const auto& pair = seed_pairs[i % seed_pairs.size()];
    EvaluationQuery q;
    q.id = static_cast<uint32_t>(i + 1);
    // Add distinct suffix if beyond seed set
    if (i >= seed_pairs.size()) {
      q.prompt = pair.base + " (variation #" + std::to_string(i / seed_pairs.size()) + ")";
    } else {
      q.prompt = pair.base;
    }
    q.is_paraphrase = false;
    q.base_query_id = std::nullopt;
    q.category = pair.category;
    q.expected_tier = "upstream";
    dataset.push_back(std::move(q));
  }

  // 2. Generate paraphrased queries referencing earlier base queries
  for (size_t i = 0; i < paraphrase_count; ++i) {
    uint32_t target_base_idx = static_cast<uint32_t>(i % base_count);
    const auto& pair = seed_pairs[target_base_idx % seed_pairs.size()];

    EvaluationQuery q;
    q.id = static_cast<uint32_t>(base_count + i + 1);
    if (target_base_idx >= seed_pairs.size()) {
      q.prompt = pair.paraphrase + " (variation #" + std::to_string(target_base_idx / seed_pairs.size()) + ")";
    } else {
      q.prompt = pair.paraphrase;
    }
    q.is_paraphrase = true;
    q.base_query_id = target_base_idx + 1;
    q.category = pair.category;
    q.expected_tier = "semantic"; // Should hit Tier 2 semantic cache!
    dataset.push_back(std::move(q));
  }

  return dataset;
}

}  // namespace tessera
