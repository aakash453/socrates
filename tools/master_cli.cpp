// tools/master_cli.cpp
// Socrates Master CLI — runs on macOS (master).
// Exposes HTTP API so any device can send prompts.
// Windows workers connect to this master.
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "socrates_c.h"

static socrates_runtime_t g_rt = nullptr;
static std::atomic<bool> g_running{true};
static const int kPort = 8080;

// ── Minimal HTTP server ────────────────────────────────────────────────
static std::string read_line(int fd) {
  std::string line;
  char c;
  while (read(fd, &c, 1) > 0) {
    if (c == '\n') break;
    if (c != '\r') line += c;
  }
  return line;
}

static void send_response(int fd, int code, const std::string& type, const std::string& body) {
  std::ostringstream r;
  r << "HTTP/1.1 " << code << " OK\r\n"
    << "Content-Type: " << type << "\r\n"
    << "Content-Length: " << body.size() << "\r\n"
    << "Access-Control-Allow-Origin: *\r\n"
    << "Connection: close\r\n\r\n"
    << body;
  std::string resp = r.str();
  write(fd, resp.c_str(), resp.size());
}

static void send_sse(int fd, const std::string& data) {
  std::string msg = "data: " + data + "\n\n";
  write(fd, msg.c_str(), msg.size());
}

static void handle_client(int fd) {
  std::string method, path;
  std::string line = read_line(fd);
  {
    std::istringstream iss(line);
    iss >> method >> path;
  }

  // Read headers
  int content_len = 0;
  while (!(line = read_line(fd)).empty()) {
    if (line.find("Content-Length:") == 0)
      content_len = std::stoi(line.substr(15));
  }

  // Read body
  std::string body(content_len, '\0');
  if (content_len > 0) read(fd, &body[0], content_len);

  // ── Route: GET /health ──────────────────────────────────────────────
  if (path == "/health") {
    send_response(fd, 200, "application/json",
      "{\"status\":\"running\",\"version\":\"" + std::string(socrates_version()) + "\"}");
  }
  // ── Route: GET /cluster ─────────────────────────────────────────────
  else if (path == "/cluster") {
    char* json = socrates_cluster_snapshot_json(g_rt);
    send_response(fd, 200, "application/json", json ? json : "{}");
    if (json) free(json);
  }
  // ── Route: GET /models ──────────────────────────────────────────────
  else if (path == "/models") {
    char* json = socrates_available_models_json(g_rt);
    send_response(fd, 200, "application/json", json ? json : "[]");
    if (json) free(json);
  }
  // ── Route: POST /join ───────────────────────────────────────────────
  else if (path == "/join" && method == "POST") {
    socrates_error_t err = socrates_join_cluster(g_rt);
    send_response(fd, err.code == 0 ? 200 : 500, "application/json",
      std::string("{\"ok\":") + (err.code == 0 ? "true" : "false") + "}");
  }
  // ── Route: POST /generate ───────────────────────────────────────────
  else if (path == "/generate" && method == "POST") {
    // Parse JSON body: {"model_id":"...","prompt":"...","max_tokens":100}
    std::string model_id = "default", prompt = "Hello";
    int max_tokens = 50;

    auto extract = [&](const std::string& key) -> std::string {
      auto pos = body.find("\"" + key + "\":");
      if (pos == std::string::npos) return "";
      pos = body.find("\"", pos + key.size() + 3);
      if (pos == std::string::npos) return "";
      auto end = body.find("\"", pos + 1);
      return body.substr(pos + 1, end - pos - 1);
    };
    auto extract_int = [&](const std::string& key) -> int {
      auto pos = body.find("\"" + key + "\":");
      if (pos == std::string::npos) return 0;
      pos += key.size() + 3;
      return std::stoi(body.substr(pos));
    };

    model_id = extract("model_id");
    if (model_id.empty()) model_id = "qwen3-1.8b";
    prompt = extract("prompt");
    if (prompt.empty()) prompt = "Hello";
    max_tokens = extract_int("max_tokens");
    if (max_tokens == 0) max_tokens = 50;

    printf("[generate] model=%s prompt=\"%s\" max=%d\n",
           model_id.c_str(), prompt.c_str(), max_tokens);

    // SSE stream
    std::string header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/event-stream\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "Cache-Control: no-cache\r\n"
                         "Connection: keep-alive\r\n\r\n";
    write(fd, header.c_str(), header.size());

    // Use socrates_generate with a callback that sends SSE
    struct Ctx { int fd; };
    Ctx ctx{fd};

    socrates_generate(g_rt, "http-req", prompt.c_str(), static_cast<uint32_t>(max_tokens), 0.7f, 2048,
      [](const socrates_stream_event_t* ev, void* data) {
        auto* c = static_cast<Ctx*>(data);
        if (!ev) return;
        char buf[512];
        if (ev->kind == 1) { // kToken
          snprintf(buf, sizeof(buf), "{\"token\":\"%s\",\"seq\":%llu}",
                   ev->text_delta ? ev->text_delta : "", (unsigned long long)ev->sequence);
          send_sse(c->fd, buf);
        } else if (ev->kind == 3) { // kCompleted
          send_sse(c->fd, "{\"done\":true}");
        } else if (ev->kind == 4) { // kFailed
          send_sse(c->fd, "{\"error\":\"generation failed\"}");
        }
      }, &ctx, nullptr);
  }
  // ── Route: POST /cancel ─────────────────────────────────────────────
  else if (path == "/cancel" && method == "POST") {
    auto id = body.find("\"generation_id\":\"");
    if (id != std::string::npos) {
      id += 17;
      auto end = body.find("\"", id);
      std::string gen_id = body.substr(id, end - id);
      socrates_cancel_by_id(g_rt, gen_id.c_str());
    }
    send_response(fd, 200, "application/json", "{\"ok\":true}");
  }
  // ── 404 ─────────────────────────────────────────────────────────────
  else {
    send_response(fd, 404, "application/json", "{\"error\":\"not found\"}");
  }

  close(fd);
}

static void http_server() {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(kPort);
  bind(sock, (struct sockaddr*)&addr, sizeof(addr));
  listen(sock, 10);

  printf("\n========================================\n");
  printf("  Master API running on port %d\n", kPort);
  printf("  Endpoints:\n");
  printf("    GET  /health   — status\n");
  printf("    GET  /cluster  — cluster snapshot\n");
  printf("    GET  /models   — available models\n");
  printf("    POST /join     — join cluster\n");
  printf("    POST /generate — {\"model_id\",\"prompt\",\"max_tokens\"}\n");
  printf("    POST /cancel   — {\"generation_id\"}\n");
  printf("========================================\n\n");

  while (g_running) {
    int client = accept(sock, nullptr, nullptr);
    if (client < 0) continue;
    std::thread(handle_client, client).detach();
  }
  close(sock);
}

// ── Main ────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
  bool no_discovery = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--no-discovery") == 0) no_discovery = true;
  }

  printf("=== Socrates Master ===\n");
  printf("Version: %s\n\n", socrates_version());

  // Init runtime as master (not leech)
  socrates_config_t cfg;
  cfg.version = SOCRATES_API_VERSION;
  cfg.state_directory = "/tmp/socrates-master";
  cfg.model_root = "/tmp/socrates-master/models";
  cfg.cluster_id = "socrates-cluster";
  cfg.trust_mode = 0;
  cfg.tracing_enabled = false;
  cfg.start_as_leech = false;  // master = participant from start
  cfg.production_mode = false;
  cfg.batching_mode = 0;
  cfg.max_batch_size = 4;
  cfg.batch_timeout_ms = 5;
  cfg.long_context_threshold = 4096;
  cfg.skip_discovery = no_discovery;

  std::filesystem::create_directories("/tmp/socrates-master/models");

  // Download models (same as worker)
  printf("\n=== Downloading models ===\n");
  struct { const char* id; const char* name; const char* url; const char* fn; } models[] = {
    {"qwen3-1.8b", "Qwen 2.5 1.5B", "https://huggingface.co/bartowski/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/Qwen2.5-1.5B-Instruct-Q4_K_M.gguf", "Qwen2.5-1.5B-Instruct-Q4_K_M.gguf"},
    {"qwen3-4b", "Qwen 2.5 3B", "https://huggingface.co/bartowski/Qwen2.5-3B-Instruct-GGUF/resolve/main/Qwen2.5-3B-Instruct-Q4_K_M.gguf", "Qwen2.5-3B-Instruct-Q4_K_M.gguf"},
    {"qwen3-6b", "Qwen 2.5 7B", "https://huggingface.co/bartowski/Qwen2.5-7B-Instruct-GGUF/resolve/main/Qwen2.5-7B-Instruct-Q4_K_M.gguf", "Qwen2.5-7B-Instruct-Q4_K_M.gguf"},
    {"llama3-8b", "Llama 3 8B", "https://huggingface.co/bartowski/Meta-Llama-3-8B-Instruct-GGUF/resolve/main/Meta-Llama-3-8B-Instruct-Q4_K_M.gguf", "Meta-Llama-3-8B-Instruct-Q4_K_M.gguf"},
    {"gemma12b", "Gemma 2 9B", "https://huggingface.co/bartowski/gemma-2-9b-it-GGUF/resolve/main/gemma-2-9b-it-Q4_K_M.gguf", "gemma-2-9b-it-Q4_K_M.gguf"},
    {"gemma26b", "Gemma 2 27B", "https://huggingface.co/bartowski/gemma-2-27b-it-GGUF/resolve/main/gemma-2-27b-it-Q4_K_M.gguf", "gemma-2-27b-it-Q4_K_M.gguf"},
    {"socrates-debug-profiler", "Profiler", "https://huggingface.co/bartowski/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf", "Qwen2.5-0.5B-Instruct-Q4_K_M.gguf"},
  };
  for (auto& m : models) {
    std::string path = "/tmp/socrates-master/models/" + std::string(m.fn);
    if (std::filesystem::exists(path)) {
      printf("  %s — already downloaded\n", m.name);
      continue;
    }
    printf("  %s — downloading...\n", m.name);
    std::string cmd = "curl -L -o \"" + path + "\" --progress-bar \"" + std::string(m.url) + "\" 2>&1";
    system(cmd.c_str());
  }
  printf("Downloads complete.\n\n");

  socrates_error_t err = socrates_init(cfg, &g_rt);
  if (err.code != 0) { printf("Init failed\n"); return 1; }
  err = socrates_start(g_rt);
  if (err.code != 0) { printf("Start failed\n"); return 1; }

  printf("Runtime started as master.\n");
  // socrates_join_cluster(g_rt);  // TODO: fix deadlock, then re-enable

  // Start HTTP server (blocks until shutdown)
  http_server();

  socrates_shutdown(g_rt);
  return 0;
}
