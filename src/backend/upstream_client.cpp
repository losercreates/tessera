#include "tessera/backend/upstream_client.hpp"
#include "tessera/common/logger.hpp"
#include <chrono>
#include <sstream>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace tessera {

static std::string DecodeChunkedBody(std::string_view chunked_data) {
  std::string decoded;
  size_t pos = 0;
  while (pos < chunked_data.size()) {
    auto line_end = chunked_data.find("\r\n", pos);
    if (line_end == std::string_view::npos) break;
    std::string hex_str = std::string(chunked_data.substr(pos, line_end - pos));
    auto semi = hex_str.find(';');
    if (semi != std::string::npos) hex_str = hex_str.substr(0, semi);
    if (hex_str.empty()) break;
    size_t chunk_len = 0;
    try {
      chunk_len = std::stoul(hex_str, nullptr, 16);
    } catch (...) {
      break;
    }
    if (chunk_len == 0) break;
    pos = line_end + 2;
    if (pos + chunk_len > chunked_data.size()) {
      decoded.append(chunked_data.substr(pos));
      break;
    }
    decoded.append(chunked_data.substr(pos, chunk_len));
    pos += chunk_len + 2;
  }
  return decoded;
}

HttpUpstreamClient::HttpUpstreamClient(UpstreamProviderConfig config)
    : config_(std::move(config)) {}

std::string_view HttpUpstreamClient::ProviderName() const noexcept {
  return config_.name;
}

std::string_view HttpUpstreamClient::Endpoint() const noexcept {
  return config_.endpoint;
}

std::string_view HttpUpstreamClient::DefaultModel() const noexcept {
  return config_.default_model;
}

std::string HttpUpstreamClient::SerializeOpenAIRequest(const UpstreamRequest& req) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"model\": \"" << (req.model.empty() ? "default" : req.model) << "\",\n";
  oss << "  \"messages\": [\n";
  for (size_t i = 0; i < req.messages.size(); ++i) {
    const auto& msg = req.messages[i];
    oss << "    {\"role\": \"" << msg.role << "\", \"content\": \"";
    for (char c : msg.content) {
      if (c == '"') oss << "\\\"";
      else if (c == '\\') oss << "\\\\";
      else if (c == '\n') oss << "\\n";
      else if (c == '\r') oss << "\\r";
      else if (c == '\t') oss << "\\t";
      else oss << c;
    }
    oss << "\"}";
    if (i + 1 < req.messages.size()) oss << ",";
    oss << "\n";
  }
  oss << "  ],\n";
  oss << "  \"temperature\": " << req.temperature << ",\n";
  oss << "  \"max_tokens\": " << req.max_tokens << "\n";
  oss << "}";
  return oss.str();
}

UpstreamResponse HttpUpstreamClient::ParseOpenAIResponse(
    int status_code,
    std::string_view body,
    double latency_ms) {
  UpstreamResponse resp;
  resp.status_code = status_code;
  resp.body = std::string(body);
  resp.latency_ms = latency_ms;

  if (status_code >= 200 && status_code < 300) {
    // Extract choice message content with escape sequence handling
    auto content_pos = body.find("\"content\":");
    if (content_pos != std::string_view::npos) {
      auto start_quote = body.find('"', content_pos + 10);
      if (start_quote != std::string_view::npos) {
        size_t end_quote = start_quote + 1;
        bool escaped = false;
        while (end_quote < body.size()) {
          char c = body[end_quote];
          if (c == '\\') {
            escaped = !escaped;
          } else if (c == '"' && !escaped) {
            break;
          } else {
            escaped = false;
          }
          end_quote++;
        }
        if (end_quote < body.size()) {
          std::string raw_content = std::string(body.substr(start_quote + 1, end_quote - start_quote - 1));
          std::string unescaped;
          unescaped.reserve(raw_content.size());
          for (size_t i = 0; i < raw_content.size(); ++i) {
            if (raw_content[i] == '\\' && i + 1 < raw_content.size()) {
              char next = raw_content[++i];
              if (next == 'n') unescaped += '\n';
              else if (next == 'r') unescaped += '\r';
              else if (next == 't') unescaped += '\t';
              else if (next == '"') unescaped += '"';
              else if (next == '\\') unescaped += '\\';
              else { unescaped += '\\'; unescaped += next; }
            } else {
              unescaped += raw_content[i];
            }
          }
          resp.completion_text = std::move(unescaped);
        }
      }
    }

    // Extract model
    auto model_pos = body.find("\"model\":");
    if (model_pos != std::string_view::npos) {
      auto start_quote = body.find('"', model_pos + 8);
      if (start_quote != std::string_view::npos) {
        auto end_quote = body.find('"', start_quote + 1);
        if (end_quote != std::string_view::npos) {
          resp.model = std::string(body.substr(start_quote + 1, end_quote - start_quote - 1));
        }
      }
    }

    // Extract usage tokens if reported by upstream API
    auto prompt_tok_pos = body.find("\"prompt_tokens\":");
    if (prompt_tok_pos != std::string_view::npos) {
      size_t num_start = prompt_tok_pos + 16;
      while (num_start < body.size() && (body[num_start] == ' ' || body[num_start] == ':')) num_start++;
      resp.prompt_tokens = static_cast<uint32_t>(std::strtoul(body.data() + num_start, nullptr, 10));
    } else {
      resp.prompt_tokens = 32;
    }

    auto compl_tok_pos = body.find("\"completion_tokens\":");
    if (compl_tok_pos != std::string_view::npos) {
      size_t num_start = compl_tok_pos + 20;
      while (num_start < body.size() && (body[num_start] == ' ' || body[num_start] == ':')) num_start++;
      resp.completion_tokens = static_cast<uint32_t>(std::strtoul(body.data() + num_start, nullptr, 10));
    } else {
      resp.completion_tokens = 16;
    }

    auto total_tok_pos = body.find("\"total_tokens\":");
    if (total_tok_pos != std::string_view::npos) {
      size_t num_start = total_tok_pos + 15;
      while (num_start < body.size() && (body[num_start] == ' ' || body[num_start] == ':')) num_start++;
      resp.total_tokens = static_cast<uint32_t>(std::strtoul(body.data() + num_start, nullptr, 10));
    } else {
      resp.total_tokens = resp.prompt_tokens + resp.completion_tokens;
    }
  } else {
    // Error extraction
    auto err_pos = body.find("\"message\":");
    if (err_pos != std::string_view::npos) {
      auto start_quote = body.find('"', err_pos + 10);
      if (start_quote != std::string_view::npos) {
        auto end_quote = body.find('"', start_quote + 1);
        if (end_quote != std::string_view::npos) {
          resp.error_message = std::string(body.substr(start_quote + 1, end_quote - start_quote - 1));
        }
      }
    }
    if (resp.error_message.empty()) {
      resp.error_message = "Upstream HTTP error status " + std::to_string(status_code);
    }
  }

  return resp;
}

UpstreamResponse HttpUpstreamClient::Execute(const UpstreamRequest& request) {
  auto start_time = std::chrono::high_resolution_clock::now();
  std::string model_name = request.model.empty() ? config_.default_model : request.model;

  // If no API key configured, use simulated offline synthesis for CI and local zero-cost testing
  if (config_.api_key.empty()) {
    std::string req_json = SerializeOpenAIRequest(request);
    TESSERA_LOG_DEBUG("Synthesizing offline mock response for {} ({})", config_.name, model_name);

    auto end_time = std::chrono::high_resolution_clock::now();
    double latency = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    std::string synthesized_body =
        std::string("{\"id\":\"cmpl-") + request.correlation_id +
        "\",\"model\":\"" + model_name +
        "\",\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Response generated by " +
        config_.name + " (" + model_name +
        ")\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":25,\"completion_tokens\":12,\"total_tokens\":37}}";

    return ParseOpenAIResponse(200, synthesized_body, latency);
  }

  // Live upstream HTTPS request via OpenSSL TLS
  std::string url = config_.endpoint;
  std::string host;
  uint16_t port = 443;
  std::string path = "/";

  if (url.rfind("https://", 0) == 0) {
    size_t slash = url.find('/', 8);
    if (slash != std::string::npos) {
      host = url.substr(8, slash - 8);
      path = url.substr(slash);
    } else {
      host = url.substr(8);
    }
  } else if (url.rfind("http://", 0) == 0) {
    port = 80;
    size_t slash = url.find('/', 7);
    if (slash != std::string::npos) {
      host = url.substr(7, slash - 7);
      path = url.substr(slash);
    } else {
      host = url.substr(7);
    }
  } else {
    host = url;
  }

  size_t colon = host.find(':');
  if (colon != std::string::npos) {
    port = static_cast<uint16_t>(std::stoi(host.substr(colon + 1)));
    host = host.substr(0, colon);
  }

  struct addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  std::string port_str = std::to_string(port);
  if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
    TESSERA_LOG_ERROR("Failed to resolve upstream host: {}", host);
    return ParseOpenAIResponse(502, "{\"error\":{\"message\":\"DNS resolution failed for upstream host\"}}", 0.0);
  }

  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) {
    freeaddrinfo(res);
    return ParseOpenAIResponse(502, "{\"error\":{\"message\":\"Socket creation failed\"}}", 0.0);
  }

  struct timeval tv{15, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
    close(fd);
    freeaddrinfo(res);
    TESSERA_LOG_ERROR("TCP connection timed out to upstream {}:{}", host, port);
    return ParseOpenAIResponse(504, "{\"error\":{\"message\":\"TCP connection timed out\"}}", 0.0);
  }
  freeaddrinfo(res);

  const SSL_METHOD* method = TLS_client_method();
  SSL_CTX* ctx = SSL_CTX_new(method);
  SSL_CTX_set_default_verify_paths(ctx);
  SSL* ssl = SSL_new(ctx);
  SSL_set_fd(ssl, fd);
  SSL_set_tlsext_host_name(ssl, host.c_str());

  if (SSL_connect(ssl) <= 0) {
    TESSERA_LOG_ERROR("TLS handshake failed with upstream {}", host);
    SSL_free(ssl);
    close(fd);
    SSL_CTX_free(ctx);
    return ParseOpenAIResponse(502, "{\"error\":{\"message\":\"TLS handshake failed\"}}", 0.0);
  }

  std::string req_json = SerializeOpenAIRequest(request);
  std::ostringstream req;
  req << "POST " << path << " HTTP/1.1\r\n";
  req << "Host: " << host << "\r\n";
  req << "User-Agent: TesseraGateway/0.1\r\n";
  req << "Accept: application/json\r\n";
  req << "Content-Type: application/json\r\n";
  req << "Authorization: Bearer " << config_.api_key << "\r\n";
  req << "Content-Length: " << req_json.size() << "\r\n";
  req << "Connection: close\r\n\r\n";
  req << req_json;

  std::string req_str = req.str();
  SSL_write(ssl, req_str.data(), static_cast<int>(req_str.size()));

  std::string raw_response;
  char buf[4096];
  int bytes = 0;
  while ((bytes = SSL_read(ssl, buf, sizeof(buf))) > 0) {
    raw_response.append(buf, static_cast<size_t>(bytes));
  }

  SSL_shutdown(ssl);
  SSL_free(ssl);
  close(fd);
  SSL_CTX_free(ctx);

  auto header_end = raw_response.find("\r\n\r\n");
  std::string headers;
  std::string body;
  if (header_end != std::string::npos) {
    headers = raw_response.substr(0, header_end);
    body = raw_response.substr(header_end + 4);
  } else {
    body = raw_response;
  }

  int status_code = 502;
  auto first_line_end = raw_response.find("\r\n");
  if (first_line_end != std::string::npos) {
    std::string first_line = raw_response.substr(0, first_line_end);
    auto space1 = first_line.find(' ');
    if (space1 != std::string::npos) {
      auto space2 = first_line.find(' ', space1 + 1);
      if (space2 != std::string::npos) {
        try {
          status_code = std::stoi(first_line.substr(space1 + 1, space2 - space1 - 1));
        } catch (...) {}
      }
    }
  }

  if (headers.find("chunked") != std::string::npos || headers.find("Chunked") != std::string::npos) {
    body = DecodeChunkedBody(body);
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  double latency = std::chrono::duration<double, std::milli>(end_time - start_time).count();

  TESSERA_LOG_INFO("Upstream {} responded with status {} in {:.1f}ms", config_.name, status_code, latency);
  return ParseOpenAIResponse(status_code, body, latency);
}

}  // namespace tessera
