# Provider Configuration Contract

**Feature**: `007-provider-agnostic-routing`  
**Contract Version**: 1.0.0  
**Format**: Declarative JSON / Environment Variable Conventions  

---

## 1. Structured JSON Configuration Schema

When loading providers from structured configuration (`tessera.json` or inline config):

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "TesseraProviderConfig",
  "type": "object",
  "properties": {
    "providers": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["name", "endpoint", "default_model"],
        "properties": {
          "name": {
            "type": "string",
            "pattern": "^[a-z0-9_-]+$",
            "description": "Unique identifier for this upstream provider"
          },
          "endpoint": {
            "type": "string",
            "format": "uri",
            "description": "Complete HTTP or HTTPS endpoint for chat completions"
          },
          "api_key": {
            "type": "string",
            "description": "Bearer token or API credential (optional for local models)"
          },
          "default_model": {
            "type": "string",
            "description": "Default model name served by this platform"
          },
          "supported_models": {
            "type": "array",
            "items": { "type": "string" },
            "description": "Additional model IDs supported by this provider"
          },
          "circuit_breaker": {
            "type": "object",
            "properties": {
              "failure_threshold": { "type": "integer", "default": 3 },
              "base_cooldown_ms": { "type": "integer", "default": 1000 },
              "max_cooldown_ms": { "type": "integer", "default": 60000 },
              "trip_on_429": { "type": "boolean", "default": true }
            }
          },
          "rate_limits": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["window_name", "capacity", "refill_rate_per_sec"],
              "properties": {
                "window_name": { "type": "string" },
                "capacity": { "type": "number" },
                "refill_rate_per_sec": { "type": "number" },
                "ttl_seconds": { "type": "integer" }
              }
            }
          }
        }
      }
    }
  }
}
```

---

## 2. Environment Variable Conventions

In cloud or Docker environments, providers can be configured via environment variables:

```bash
# Comma-separated list of active provider names
TESSERA_PROVIDERS="ollama,groq,deepseek,openrouter"

# Provider 1: Local Ollama runtime (no API key required)
PROVIDER_OLLAMA_ENDPOINT="http://localhost:11434/v1/chat/completions"
PROVIDER_OLLAMA_MODEL="llama3.2"

# Provider 2: Groq high-speed cloud
PROVIDER_GROQ_ENDPOINT="https://api.groq.com/openai/v1/chat/completions"
PROVIDER_GROQ_KEY="gsk_..."
PROVIDER_GROQ_MODEL="llama-3.1-8b-instant"

# Provider 3: DeepSeek
PROVIDER_DEEPSEEK_ENDPOINT="https://api.deepseek.com/v1/chat/completions"
PROVIDER_DEEPSEEK_KEY="sk-..."
PROVIDER_DEEPSEEK_MODEL="deepseek-chat"

# Provider 4: OpenRouter Aggregator
PROVIDER_OPENROUTER_ENDPOINT="https://openrouter.ai/api/v1/chat/completions"
PROVIDER_OPENROUTER_KEY="sk-or-..."
PROVIDER_OPENROUTER_MODEL="openrouter/free"
```
