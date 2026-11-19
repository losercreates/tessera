#include "tessera/routing/provider_registry.hpp"
#include "tessera/common/logger.hpp"
#include "tessera/telemetry/telemetry_registry.hpp"
#include "tessera/ratelimit/rate_limiter.hpp"
#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace tessera {

static std::string ToLower(std::string_view sv) {
  std::string s;
  s.reserve(sv.size());
  for (char c : sv) {
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return s;
}

static std::string ToUpper(std::string_view sv) {
  std::string s;
  s.reserve(sv.size());
  for (char c : sv) {
    s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return s;
}

static std::vector<std::string> SplitComma(std::string_view sv) {
  std::vector<std::string> tokens;
  size_t start = 0;
  while (start < sv.size()) {
    while (start < sv.size() && (sv[start] == ' ' || sv[start] == '\t' || sv[start] == ',')) {
      start++;
    }
    if (start >= sv.size()) break;
    size_t end = sv.find(',', start);
    if (end == std::string_view::npos) {
      end = sv.size();
    }
    size_t token_end = end;
    while (token_end > start && (sv[token_end - 1] == ' ' || sv[token_end - 1] == '\t')) {
      token_end--;
    }
    if (token_end > start) {
      tokens.emplace_back(sv.substr(start, token_end - start));
    }
    start = end + 1;
  }
  return tokens;
}

ProviderRegistry::ProviderRegistry() {
  client_factory_ = [](const ProviderConfig& cfg) -> std::shared_ptr<IUpstreamClient> {
    return std::make_shared<HttpUpstreamClient>(cfg);
  };
  breaker_factory_ = [](const CircuitBreakerConfig& cfg) -> std::shared_ptr<ICircuitBreaker> {
    return std::make_shared<CircuitBreaker>(cfg);
  };
}

ProviderRegistry::ProviderRegistry(ProviderRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  provider_order_ = std::move(other.provider_order_);
  providers_ = std::move(other.providers_);
  client_factory_ = std::move(other.client_factory_);
  breaker_factory_ = std::move(other.breaker_factory_);
  rate_limiter_ = std::move(other.rate_limiter_);
}

ProviderRegistry& ProviderRegistry::operator=(ProviderRegistry&& other) noexcept {
  if (this != &other) {
    std::scoped_lock lock(mutex_, other.mutex_);
    provider_order_ = std::move(other.provider_order_);
    providers_ = std::move(other.providers_);
    client_factory_ = std::move(other.client_factory_);
    breaker_factory_ = std::move(other.breaker_factory_);
    rate_limiter_ = std::move(other.rate_limiter_);
  }
  return *this;
}

void ProviderRegistry::SetRateLimiter(std::shared_ptr<DistributedRateLimiter> rate_limiter) {
  std::lock_guard<std::mutex> lock(mutex_);
  rate_limiter_ = std::move(rate_limiter);
  if (rate_limiter_) {
    for (const auto& [name, entry] : providers_) {
      for (const auto& rule : entry.config.rate_limits) {
        rate_limiter_->AddRule(rule);
      }
    }
  }
}

void ProviderRegistry::RegisterRateLimits(DistributedRateLimiter& limiter) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [name, entry] : providers_) {
    for (const auto& rule : entry.config.rate_limits) {
      limiter.AddRule(rule);
    }
  }
}

void ProviderRegistry::SetClientFactory(ClientFactory factory) {
  std::lock_guard<std::mutex> lock(mutex_);
  client_factory_ = std::move(factory);
}

void ProviderRegistry::SetBreakerFactory(BreakerFactory factory) {
  std::lock_guard<std::mutex> lock(mutex_);
  breaker_factory_ = std::move(factory);
}

bool ProviderRegistry::RegisterProvider(const ProviderConfig& config, std::string* error_msg) {
  if (!config.Validate(error_msg)) {
    return false;
  }

  std::string lower_name = ToLower(config.name);

  std::shared_ptr<IUpstreamClient> client;
  std::shared_ptr<ICircuitBreaker> breaker;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_factory_) {
      client = client_factory_(config);
    } else {
      client = std::make_shared<HttpUpstreamClient>(config);
    }

    if (breaker_factory_) {
      breaker = breaker_factory_(config.circuit_config);
    } else {
      breaker = std::make_shared<CircuitBreaker>(config.circuit_config);
    }

    if (providers_.find(lower_name) == providers_.end()) {
      provider_order_.push_back(lower_name);
    }

    providers_[lower_name] = ProviderEntry{
        .config = config,
        .client = std::move(client),
        .breaker = std::move(breaker),
    };

    if (rate_limiter_) {
      for (const auto& rule : config.rate_limits) {
        rate_limiter_->AddRule(rule);
      }
    }
  }

  // Register in telemetry registry
  TelemetryRegistry::Instance().RegisterProvider(lower_name);

  TESSERA_LOG_INFO("Registered upstream provider '{}' (endpoint={}, default_model={}, is_local={})",
                   lower_name, config.endpoint, config.default_model, config.IsLocal());
  return true;
}

bool ProviderRegistry::UnregisterProvider(std::string_view name) {
  std::string lower_name = ToLower(name);
  std::lock_guard<std::mutex> lock(mutex_);
  if (rate_limiter_) {
    rate_limiter_->ClearRules(lower_name);
  }
  auto it = std::find(provider_order_.begin(), provider_order_.end(), lower_name);
  if (it != provider_order_.end()) {
    provider_order_.erase(it);
  }
  return providers_.erase(lower_name) > 0;
}

std::shared_ptr<IUpstreamClient> ProviderRegistry::GetProviderClient(std::string_view name) const {
  std::string lower_name = ToLower(name);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = providers_.find(lower_name);
  if (it != providers_.end()) {
    return it->second.client;
  }
  return nullptr;
}

std::shared_ptr<ICircuitBreaker> ProviderRegistry::GetProviderBreaker(std::string_view name) const {
  std::string lower_name = ToLower(name);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = providers_.find(lower_name);
  if (it != providers_.end()) {
    return it->second.breaker;
  }
  return nullptr;
}

const ProviderConfig* ProviderRegistry::GetProviderConfig(std::string_view name) const {
  std::string lower_name = ToLower(name);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = providers_.find(lower_name);
  if (it != providers_.end()) {
    return &it->second.config;
  }
  return nullptr;
}

bool ProviderRegistry::HasProvider(std::string_view name) const {
  std::string lower_name = ToLower(name);
  std::lock_guard<std::mutex> lock(mutex_);
  return providers_.find(lower_name) != providers_.end();
}

size_t ProviderRegistry::ProviderCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return providers_.size();
}

std::vector<std::string> ProviderRegistry::GetRegisteredProviders() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return provider_order_;
}

ProviderConfig ProviderRegistry::DefaultGroqConfig() {
  ProviderConfig cfg;
  cfg.name = "groq";
  cfg.endpoint = "https://api.groq.com/openai/v1/chat/completions";
  cfg.default_model = "llama-3.1-8b-instant";
  cfg.supported_models = {"llama-3.1-8b-instant", "llama-3.3-70b-versatile"};
  cfg.rate_limits = RateLimitPolicies::GroqRules();

  if (const char* k = std::getenv("GROQ_API_KEY")) {
    cfg.api_key = k;
  }
  if (const char* m = std::getenv("GROQ_MODEL")) {
    cfg.default_model = m;
  }
  return cfg;
}

ProviderConfig ProviderRegistry::DefaultOpenRouterConfig() {
  ProviderConfig cfg;
  cfg.name = "openrouter";
  cfg.endpoint = "https://openrouter.ai/api/v1/chat/completions";
  cfg.default_model = "openrouter/free";
  cfg.supported_models = {"openrouter/free", "google/gemma-2-9b-it"};
  cfg.rate_limits = RateLimitPolicies::OpenRouterRules();

  if (const char* k = std::getenv("OPENROUTER_API_KEY")) {
    cfg.api_key = k;
  }
  if (const char* m = std::getenv("OPENROUTER_MODEL")) {
    cfg.default_model = m;
  }
  return cfg;
}

ProviderRegistry ProviderRegistry::LoadFromEnvironment(bool preseed_defaults) {
  ProviderRegistry registry;

  // 1. Scan for numbered/indexed providers: PROVIDER_1_NAME, PROVIDER_2_NAME, ... PROVIDER_N_NAME
  bool found_indexed = false;
  for (int i = 1; ; ++i) {
    std::string idx = std::to_string(i);
    std::string env_name = "PROVIDER_" + idx + "_NAME";
    const char* val_name = std::getenv(env_name.c_str());
    if (!val_name || *val_name == '\0') {
      break;
    }

    found_indexed = true;
    std::string name = ToLower(val_name);
    ProviderConfig cfg;
    cfg.name = name;

    // Sane defaults for known platforms if specific endpoints/models omitted
    if (name == "groq") {
      cfg = DefaultGroqConfig();
    } else if (name == "openrouter") {
      cfg = DefaultOpenRouterConfig();
    } else if (name == "deepseek") {
      cfg.endpoint = "https://api.deepseek.com/v1/chat/completions";
      cfg.default_model = "deepseek-chat";
    } else if (name == "ollama") {
      cfg.endpoint = "http://localhost:11434/v1/chat/completions";
      cfg.default_model = "llama3.2";
    } else {
      cfg.default_model = "default";
      cfg.endpoint = "http://localhost:8000/v1/chat/completions";
    }

    std::string env_endpoint = "PROVIDER_" + idx + "_ENDPOINT";
    if (const char* val = std::getenv(env_endpoint.c_str())) {
      cfg.endpoint = val;
    }

    std::string env_key = "PROVIDER_" + idx + "_KEY";
    if (const char* val = std::getenv(env_key.c_str())) {
      cfg.api_key = val;
    } else {
      std::string env_api_key = "PROVIDER_" + idx + "_API_KEY";
      if (const char* val2 = std::getenv(env_api_key.c_str())) {
        cfg.api_key = val2;
      }
    }

    std::string env_model = "PROVIDER_" + idx + "_MODEL";
    if (const char* val = std::getenv(env_model.c_str())) {
      cfg.default_model = val;
    } else {
      std::string env_def_model = "PROVIDER_" + idx + "_DEFAULT_MODEL";
      if (const char* val2 = std::getenv(env_def_model.c_str())) {
        cfg.default_model = val2;
      }
    }

    std::string env_supported = "PROVIDER_" + idx + "_SUPPORTED_MODELS";
    if (const char* val = std::getenv(env_supported.c_str())) {
      cfg.supported_models = SplitComma(val);
    }

    std::string env_thresh = "PROVIDER_" + idx + "_FAILURE_THRESHOLD";
    if (const char* val = std::getenv(env_thresh.c_str())) {
      cfg.circuit_config.consecutive_failure_threshold = static_cast<uint32_t>(std::stoi(val));
    }

    std::string env_cooldown = "PROVIDER_" + idx + "_COOLDOWN_MS";
    if (const char* val = std::getenv(env_cooldown.c_str())) {
      cfg.circuit_config.base_cooldown_ms = static_cast<uint32_t>(std::stoi(val));
    }

    std::string env_max_cooldown = "PROVIDER_" + idx + "_MAX_COOLDOWN_MS";
    if (const char* val = std::getenv(env_max_cooldown.c_str())) {
      cfg.circuit_config.max_cooldown_ms = static_cast<uint32_t>(std::stoi(val));
    }

    // Dynamic rate limits via RPM / RPD
    std::string env_rpm = "PROVIDER_" + idx + "_RPM";
    if (const char* val = std::getenv(env_rpm.c_str())) {
      double rpm = std::stod(val);
      RateLimitRule rule;
      rule.provider = name;
      rule.window_name = "minute";
      rule.capacity = rpm;
      rule.refill_rate_per_sec = rpm / 60.0;
      rule.ttl_seconds = 60;
      cfg.rate_limits.push_back(rule);
    }

    std::string env_rpd = "PROVIDER_" + idx + "_RPD";
    if (const char* val = std::getenv(env_rpd.c_str())) {
      double rpd = std::stod(val);
      RateLimitRule rule;
      rule.provider = name;
      rule.window_name = "day";
      rule.capacity = rpd;
      rule.refill_rate_per_sec = rpd / 86400.0;
      rule.ttl_seconds = 86400;
      cfg.rate_limits.push_back(rule);
    }

    std::string err;
    if (!registry.RegisterProvider(cfg, &err)) {
      TESSERA_LOG_WARN("Skipping invalid provider '{}' (index {}) from environment: {}", name, idx, err);
    }
  }

  if (found_indexed) {
    return registry;
  }

  // 2. Named providers via TESSERA_PROVIDERS
  std::vector<std::string> provider_names;
  if (const char* env_providers = std::getenv("TESSERA_PROVIDERS")) {
    provider_names = SplitComma(env_providers);
  }

  if (!provider_names.empty()) {
    for (const auto& raw_name : provider_names) {
      std::string name = ToLower(raw_name);
      std::string upper_name = ToUpper(name);

      ProviderConfig cfg;
      cfg.name = name;
      if (name == "groq") {
        cfg = DefaultGroqConfig();
      } else if (name == "openrouter") {
        cfg = DefaultOpenRouterConfig();
      } else if (name == "deepseek") {
        cfg.endpoint = "https://api.deepseek.com/v1/chat/completions";
        cfg.default_model = "deepseek-chat";
      } else if (name == "ollama") {
        cfg.endpoint = "http://localhost:11434/v1/chat/completions";
        cfg.default_model = "llama3.2";
      } else {
        cfg.default_model = "default";
        cfg.endpoint = "http://localhost:8000/v1/chat/completions";
      }

      std::string env_endpoint = "PROVIDER_" + upper_name + "_ENDPOINT";
      if (const char* val = std::getenv(env_endpoint.c_str())) {
        cfg.endpoint = val;
      }

      std::string env_key = "PROVIDER_" + upper_name + "_KEY";
      if (const char* val = std::getenv(env_key.c_str())) {
        cfg.api_key = val;
      } else {
        std::string env_api_key = "PROVIDER_" + upper_name + "_API_KEY";
        if (const char* val2 = std::getenv(env_api_key.c_str())) {
          cfg.api_key = val2;
        }
      }

      std::string env_model = "PROVIDER_" + upper_name + "_MODEL";
      if (const char* val = std::getenv(env_model.c_str())) {
        cfg.default_model = val;
      } else {
        std::string env_def_model = "PROVIDER_" + upper_name + "_DEFAULT_MODEL";
        if (const char* val2 = std::getenv(env_def_model.c_str())) {
          cfg.default_model = val2;
        }
      }

      std::string env_supported = "PROVIDER_" + upper_name + "_SUPPORTED_MODELS";
      if (const char* val = std::getenv(env_supported.c_str())) {
        cfg.supported_models = SplitComma(val);
      }

      std::string env_thresh = "PROVIDER_" + upper_name + "_FAILURE_THRESHOLD";
      if (const char* val = std::getenv(env_thresh.c_str())) {
        cfg.circuit_config.consecutive_failure_threshold = static_cast<uint32_t>(std::stoi(val));
      }

      std::string env_cooldown = "PROVIDER_" + upper_name + "_COOLDOWN_MS";
      if (const char* val = std::getenv(env_cooldown.c_str())) {
        cfg.circuit_config.base_cooldown_ms = static_cast<uint32_t>(std::stoi(val));
      }

      std::string env_max_cooldown = "PROVIDER_" + upper_name + "_MAX_COOLDOWN_MS";
      if (const char* val = std::getenv(env_max_cooldown.c_str())) {
        cfg.circuit_config.max_cooldown_ms = static_cast<uint32_t>(std::stoi(val));
      }

      std::string env_rpm = "PROVIDER_" + upper_name + "_RPM";
      if (const char* val = std::getenv(env_rpm.c_str())) {
        double rpm = std::stod(val);
        RateLimitRule rule;
        rule.provider = name;
        rule.window_name = "minute";
        rule.capacity = rpm;
        rule.refill_rate_per_sec = rpm / 60.0;
        rule.ttl_seconds = 60;
        cfg.rate_limits.push_back(rule);
      }

      std::string env_rpd = "PROVIDER_" + upper_name + "_RPD";
      if (const char* val = std::getenv(env_rpd.c_str())) {
        double rpd = std::stod(val);
        RateLimitRule rule;
        rule.provider = name;
        rule.window_name = "day";
        rule.capacity = rpd;
        rule.refill_rate_per_sec = rpd / 86400.0;
        rule.ttl_seconds = 86400;
        cfg.rate_limits.push_back(rule);
      }

      std::string err;
      if (!registry.RegisterProvider(cfg, &err)) {
        TESSERA_LOG_WARN("Skipping invalid provider '{}' from environment: {}", name, err);
      }
    }
    return registry;
  }

  // 3. Legacy single-provider credentials (only if explicit providers were not provided)
  bool found_legacy = false;
  if (std::getenv("GROQ_API_KEY")) {
    registry.RegisterProvider(DefaultGroqConfig());
    found_legacy = true;
  }
  if (std::getenv("OPENROUTER_API_KEY")) {
    registry.RegisterProvider(DefaultOpenRouterConfig());
    found_legacy = true;
  }
  if (found_legacy) {
    return registry;
  }

  // 4. Offline mock defaults (only when zero provider config is found and preseed_defaults=true)
  if (preseed_defaults) {
    registry.RegisterProvider(DefaultGroqConfig());
    registry.RegisterProvider(DefaultOpenRouterConfig());
  }

  return registry;
}

ProviderRegistry ProviderRegistry::LoadFromJson(std::string_view json_str) {
  ProviderRegistry registry;

  // Simple, robust zero-dependency JSON extraction for providers array
  auto prov_arr_pos = json_str.find("\"providers\":");
  if (prov_arr_pos == std::string_view::npos) {
    return registry;
  }

  auto open_bracket = json_str.find('[', prov_arr_pos);
  if (open_bracket == std::string_view::npos) {
    return registry;
  }

  auto close_bracket = json_str.find(']', open_bracket);
  if (close_bracket == std::string_view::npos) {
    return registry;
  }

  std::string_view array_content = json_str.substr(open_bracket + 1, close_bracket - open_bracket - 1);
  size_t pos = 0;

  while (pos < array_content.size()) {
    auto obj_start = array_content.find('{', pos);
    if (obj_start == std::string_view::npos) break;
    auto obj_end = array_content.find('}', obj_start);
    if (obj_end == std::string_view::npos) break;

    std::string_view obj = array_content.substr(obj_start, obj_end - obj_start + 1);
    ProviderConfig cfg;

    auto extract_string = [&](std::string_view key) -> std::string {
      std::string pattern = "\"" + std::string(key) + "\":";
      auto kpos = obj.find(pattern);
      if (kpos == std::string_view::npos) return "";
      auto q1 = obj.find('"', kpos + pattern.size());
      if (q1 == std::string_view::npos) return "";
      auto q2 = obj.find('"', q1 + 1);
      if (q2 == std::string_view::npos) return "";
      return std::string(obj.substr(q1 + 1, q2 - q1 - 1));
    };

    cfg.name = extract_string("name");
    cfg.endpoint = extract_string("endpoint");
    cfg.api_key = extract_string("api_key");
    cfg.default_model = extract_string("default_model");

    // Optional circuit breaker settings
    auto extract_uint = [&](std::string_view key) -> std::optional<uint32_t> {
      std::string pattern = "\"" + std::string(key) + "\":";
      auto kpos = obj.find(pattern);
      if (kpos == std::string_view::npos) return std::nullopt;
      auto val_start = obj.find_first_not_of(" \t\n\r", kpos + pattern.size());
      if (val_start == std::string_view::npos) return std::nullopt;
      auto val_end = obj.find_first_of(",}\n\r", val_start);
      if (val_end == std::string_view::npos) val_end = obj.size();
      try {
        return static_cast<uint32_t>(std::stoul(std::string(obj.substr(val_start, val_end - val_start))));
      } catch (...) {
        return std::nullopt;
      }
    };

    if (auto thresh = extract_uint("failure_threshold")) {
      cfg.circuit_config.consecutive_failure_threshold = *thresh;
    }
    if (auto cooldown = extract_uint("base_cooldown_ms")) {
      cfg.circuit_config.base_cooldown_ms = *cooldown;
    }
    if (auto max_cd = extract_uint("max_cooldown_ms")) {
      cfg.circuit_config.max_cooldown_ms = *max_cd;
    }

    // Optional rate limits
    auto extract_double = [&](std::string_view key) -> std::optional<double> {
      std::string pattern = "\"" + std::string(key) + "\":";
      auto kpos = obj.find(pattern);
      if (kpos == std::string_view::npos) return std::nullopt;
      auto val_start = obj.find_first_not_of(" \t\n\r", kpos + pattern.size());
      if (val_start == std::string_view::npos) return std::nullopt;
      auto val_end = obj.find_first_of(",}\n\r", val_start);
      if (val_end == std::string_view::npos) val_end = obj.size();
      try {
        return std::stod(std::string(obj.substr(val_start, val_end - val_start)));
      } catch (...) {
        return std::nullopt;
      }
    };

    if (auto rpm = extract_double("rpm")) {
      RateLimitRule rule;
      rule.provider = cfg.name;
      rule.window_name = "minute";
      rule.capacity = *rpm;
      rule.refill_rate_per_sec = *rpm / 60.0;
      rule.ttl_seconds = 60;
      cfg.rate_limits.push_back(rule);
    }
    if (auto rpd = extract_double("rpd")) {
      RateLimitRule rule;
      rule.provider = cfg.name;
      rule.window_name = "day";
      rule.capacity = *rpd;
      rule.refill_rate_per_sec = *rpd / 86400.0;
      rule.ttl_seconds = 86400;
      cfg.rate_limits.push_back(rule);
    }

    std::string err;
    registry.RegisterProvider(cfg, &err);
    pos = obj_end + 1;
  }

  return registry;
}

}  // namespace tessera
