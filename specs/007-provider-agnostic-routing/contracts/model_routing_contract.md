# Model Routing & Cascade Contract

**Feature**: `007-provider-agnostic-routing`  
**Contract Version**: 1.0.0  
**Format**: Declarative Routing & Cascading Specification  

---

## 1. Model Route Mapping Schema

Defines how incoming chat completion model requests are resolved to provider cascade sequences:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "TesseraModelRoutingConfig",
  "type": "object",
  "properties": {
    "default_cascade": {
      "type": "array",
      "items": { "type": "string" },
      "description": "Fallback cascade sequence used when model is unspecified or unmapped"
    },
    "routes": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["model", "cascade"],
        "properties": {
          "model": {
            "type": "string",
            "description": "Exact model ID (e.g. 'deepseek-chat') or prefix pattern (e.g. 'ollama/*')"
          },
          "cascade": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["provider"],
              "properties": {
                "provider": { "type": "string" },
                "model_override": {
                  "type": "string",
                  "description": "Remap model name when dispatching to this provider (optional)"
                }
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

## 2. Example Concrete Routing Configuration

```json
{
  "default_cascade": [
    { "provider": "groq", "model_override": "llama-3.1-8b-instant" },
    { "provider": "openrouter", "model_override": "openrouter/free" }
  ],
  "routes": [
    {
      "model": "deepseek-chat",
      "cascade": [
        { "provider": "deepseek", "model_override": "deepseek-chat" },
        { "provider": "openrouter", "model_override": "deepseek/deepseek-chat" },
        { "provider": "groq", "model_override": "llama-3.1-8b-instant" }
      ]
    },
    {
      "model": "llama3.2",
      "cascade": [
        { "provider": "ollama", "model_override": "llama3.2" },
        { "provider": "groq", "model_override": "llama-3.1-8b-instant" }
      ]
    },
    {
      "model": "gpt-4o",
      "cascade": [
        { "provider": "openai", "model_override": "gpt-4o" },
        { "provider": "openrouter", "model_override": "openai/gpt-4o" }
      ]
    }
  ]
}
```

---

## 3. Resolution Semantics

1. **Exact Match**: If `request.model` strictly equals a rule's `model` field, execute that rule's cascade.
2. **Provider Prefix Match**: If `request.model` starts with `<provider>/<model_id>` (e.g., `ollama/phi3` or `groq/llama-3.1-8b-instant`), route to `<provider>` with the prefix stripped.
3. **Default Catch-All**: If no rule matches, route through `default_cascade`. If `default_cascade` is exhausted or unconfigured, return HTTP 400 Bad Request with known model options.
