# Data Model: Redis Exact-Match Cache & Distributed Rate Limiting

**Feature**: `002-redis-exact-cache-ratelimit`  
**Date**: 2026-11-02  
**Status**: Completed  

---

## Redis Key Schema & Storage Formats

### 1. Tier 1 Exact-Match Cache Entry

* **Key Pattern**: `cache:exact:<sha256_hash>`  
  * Example: `cache:exact:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
* **Type**: `STRING` (Serialized JSON / binary payload)
* **TTL**: Configurable (Default: `86400` seconds / 24 hours)
* **Value Payload Schema**:

```json
{
  "id": "chatcmpl-d40d7a3e-73ec-42d6-a3d8-d774f4482cab",
  "created": 1730510000,
  "model": "llama-3.3-70b",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "A distributed system consists of autonomous nodes communicating over a network."
      },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 15,
    "completion_tokens": 18,
    "total_tokens": 33
  },
  "cached_at_ms": 1730510001000
}
```

---

### 2. Distributed Token Bucket Rate Limit State

* **Key Pattern**: `ratelimit:<provider>:<window>`  
  * Examples:  
    * `ratelimit:groq:minute` (30 tokens capacity, 0.5 tokens/sec refill rate, 60s TTL)  
    * `ratelimit:groq:day` (1,000 tokens capacity, 0.01157 tokens/sec refill rate, 86400s TTL)  
    * `ratelimit:openrouter:minute` (20 tokens capacity, 0.333 tokens/sec refill rate, 60s TTL)  
    * `ratelimit:openrouter:day` (50 tokens capacity, 0.000578 tokens/sec refill rate, 86400s TTL)  
* **Type**: `HASH`
* **Fields**:

| Field | Type | Description |
|---|---|---|
| `tokens` | `string` (float) | Current available tokens remaining in the bucket |
| `last_refill_ms` | `string` (integer) | Millisecond epoch timestamp of last replenishment calculation |

---

## Domain Structs & C++ Schemas

### 1. `RateLimitRule`
Defines the quota policy for a provider window.

| Attribute | Type | Description |
|---|---|---|
| `provider` | `string` | Provider identifier (`"groq"`, `"openrouter"`) |
| `window_name` | `string` | Window name (`"minute"`, `"day"`) |
| `capacity` | `uint32_t` | Burst capacity (e.g. 30 for Groq minute) |
| `refill_rate_per_sec` | `double` | Replenishment rate per second |
| `ttl_seconds` | `uint32_t` | Key expiration TTL |

---

### 2. `RateLimitResult`
Output returned by the rate limit evaluation engine.

| Attribute | Type | Description |
|---|---|---|
| `allowed` | `bool` | True if request was admitted; false if throttled |
| `remaining_tokens` | `uint32_t` | Tokens left in bucket after evaluation |
| `retry_after_ms` | `uint64_t` | Milliseconds until sufficient tokens replenish |
| `provider` | `string` | Provider being evaluated |

---

### 3. `CacheLookupResult`
Result of a Tier 1 exact cache query.

| Attribute | Type | Description |
|---|---|---|
| `hit` | `bool` | True if response was found in cache and unexpired |
| `response` | `ChatCompletionResponse` | Deserialized response payload |
| `cache_tier` | `string` | `"exact"` for Tier 1 |
| `latency_ms` | `double` | Time taken to query and deserialize |

