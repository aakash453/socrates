// tools/worker_cli.cpp
// Socrates Worker CLI — runs on Mac/Windows laptops.
// Downloads models, waits for master (phone), then joins the cluster.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "socrates_c.h"

static socrates_runtime_t g_rt = nullptr;
static std::atomic<bool> g_running{true};
static bool g_standalone = false;
static bool g_no_discovery = false;
static bool g_skip_downloads = false;
static bool g_joined = false;

static void print_json(const char* json) {
  if (json) { printf("%s\n", json); free((void*)json); }
  else printf("null\n");
}

// ── Real model download via curl ───────────────────────────────────────
struct ModelInfo {
  const char* id;
  const char* name;
  const char* url;
  const char* filename;
};

static const ModelInfo kModels[] = {
  {"qwen3-1.8b", "Qwen 2.5 1.5B (INT4, ~1.0 GB)",
   "https://huggingface.co/bartowski/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/Qwen2.5-1.5B-Instruct-Q4_K_M.gguf",
   "Qwen2.5-1.5B-Instruct-Q4_K_M.gguf"},
  {"qwen3-4b",   "Qwen 2.5 3B (INT4, ~2.0 GB)",
   "https://huggingface.co/bartowski/Qwen2.5-3B-Instruct-GGUF/resolve/main/Qwen2.5-3B-Instruct-Q4_K_M.gguf",
   "Qwen2.5-3B-Instruct-Q4_K_M.gguf"},
  {"qwen3-6b",   "Qwen 2.5 7B (INT4, ~4.5 GB)",
   "https://huggingface.co/bartowski/Qwen2.5-7B-Instruct-GGUF/resolve/main/Qwen2.5-7B-Instruct-Q4_K_M.gguf",
   "Qwen2.5-7B-Instruct-Q4_K_M.gguf"},
  {"llama3-8b",  "Llama 3 8B (INT4, ~5.0 GB)",
   "https://huggingface.co/bartowski/Meta-Llama-3-8B-Instruct-GGUF/resolve/main/Meta-Llama-3-8B-Instruct-Q4_K_M.gguf",
   "Meta-Llama-3-8B-Instruct-Q4_K_M.gguf"},
  {"gemma12b",   "Gemma 2 9B (INT4, ~5.5 GB)",
   "https://huggingface.co/bartowski/gemma-2-9b-it-GGUF/resolve/main/gemma-2-9b-it-Q4_K_M.gguf",
   "gemma-2-9b-it-Q4_K_M.gguf"},
  {"gemma26b",   "Gemma 2 27B (INT4, ~16 GB)",
   "https://huggingface.co/bartowski/gemma-2-27b-it-GGUF/resolve/main/gemma-2-27b-it-Q4_K_M.gguf",
   "gemma-2-27b-it-Q4_K_M.gguf"},
  {"socrates-debug-profiler", "Socrates Profiler (INT4, ~0.1 GB)",
   "https://huggingface.co/bartowski/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf",
   "Qwen2.5-0.5B-Instruct-Q4_K_M.gguf"},
};

static void download_models(const std::string& model_dir) {
  printf("\n=== Downloading models to %s ===\n", model_dir.c_str());
  std::filesystem::create_directories(model_dir);

  for (const auto& m : kModels) {
    std::string path = model_dir + "/" + m.filename;

    if (std::filesystem::exists(path)) {
      auto sz = std::filesystem::file_size(path);
      printf("  %s — already downloaded (%.1f MB)\n", m.name, static_cast<double>(sz) / 1e6);
      continue;
    }

    printf("  %s — downloading...\n", m.name);
    printf("    %s\n", m.url);

    std::string cmd = "curl -L -o \"" + path + "\" --progress-bar \"" + std::string(m.url) + "\" 2>&1";
    int ret = system(cmd.c_str());
    if (ret != 0) {
      printf("    Download failed (curl exit %d). Skipping.\n", ret);
      continue;
    }

    if (std::filesystem::exists(path)) {
      auto sz = std::filesystem::file_size(path);
      printf("    Done (%.1f MB)\n", static_cast<double>(sz) / 1e6);
    }
  }
  printf("Downloads complete.\n\n");
}

// ── Wait for master ────────────────────────────────────────────────────
static std::string wait_for_master() {
  printf("Waiting for master (phone) to join the cluster...\n");
  int dots = 0;
  while (g_running) {
    char* json = socrates_cluster_snapshot_json(g_rt);
    std::string snap(json ? json : "{}");
    free((void*)json);

    auto pos = snap.find("\"leader_id\":\"");
    if (pos != std::string::npos) {
      pos += 14;
      auto end = snap.find("\"", pos);
      std::string leader = snap.substr(pos, end - pos);
      if (!leader.empty() && leader != "null") {
        printf("\rMaster detected: %s\n", leader.c_str());
        return leader;
      }
    }

    printf("\rWaiting for master");
    for (int i = 0; i < dots; ++i) printf(".");
    printf("   ");
    fflush(stdout);
    dots = (dots + 1) % 4;
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  return "";
}

static void join_cluster() {
  printf("Joining cluster as participant...\n");
  socrates_error_t err = socrates_join_cluster(g_rt);
  if (err.code == 0) {
    g_joined = true;
    printf("Joined! This machine is now participating in inference.\n");
  } else {
    printf("Join failed: %s\n", err.message ? err.message : "unknown");
  }
}

static void show_status() {
  printf("\n=== Cluster Status ===\n");
  printf("Joined: %s\n", g_joined ? "yes (participant)" : "no (leech)");
  char* snap = socrates_cluster_snapshot_json(g_rt);
  if (snap) {
    std::string s(snap);
    size_t count = 0, pos = 0;
    while ((pos = s.find("\"node_id\":", pos)) != std::string::npos) { ++count; ++pos; }
    printf("Devices in cluster: %zu\n", count);
    print_json(snap);
  }
  printf("\nModel download progress:\n");
  char* prog = socrates_model_progress_json(g_rt);
  print_json(prog);
  printf("\n");
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--standalone") == 0) g_standalone = true;
    if (strcmp(argv[i], "--no-discovery") == 0) g_no_discovery = true;
    if (strcmp(argv[i], "--skip-downloads") == 0) g_skip_downloads = true;
  }

  printf("=== Socrates Worker ===\n");
  printf("Version: %s\n\n", socrates_version());

  // 1. Download real models
  if (!g_skip_downloads) {
    download_models("/tmp/socrates-worker/models");
  } else {
    printf("Skipping model downloads (--skip-downloads).\n\n");
  }

  // 2. Init runtime
  printf("Initializing runtime...\n");
  socrates_config_t cfg;
  cfg.version = SOCRATES_API_VERSION;
  cfg.state_directory = "/tmp/socrates-worker";
  cfg.model_root = "/tmp/socrates-worker/models";
  cfg.cluster_id = "socrates-cluster";
  cfg.trust_mode = 0;
  cfg.tracing_enabled = false;
  cfg.start_as_leech = true;
  cfg.production_mode = true;
  cfg.batching_mode = 0;
  cfg.max_batch_size = 4;
  cfg.batch_timeout_ms = 5;
  cfg.long_context_threshold = 4096;
  cfg.skip_discovery = g_no_discovery;

  socrates_error_t err = socrates_init(cfg, &g_rt);
  if (err.code != 0) { printf("Init failed\n"); return 1; }
  err = socrates_start(g_rt);
  if (err.code != 0) { printf("Start failed\n"); return 1; }
  printf("Runtime started (leech mode).\n");

  // 3. Wait for master or go standalone
  if (!g_standalone) {
    std::string master = wait_for_master();
    if (master.empty()) { socrates_shutdown(g_rt); return 0; }
  } else {
    printf("Standalone mode — joining cluster immediately.\n");
    join_cluster();
  }

  // 4. CLI
  printf("\nCommands: j=join  l=leave  s=status  q=quit\n\n");
  while (g_running) {
    printf("> "); fflush(stdout);
    int c = getchar(); if (c == EOF) break;
    while (getchar() != '\n') {}
    switch (c) {
      case 'j': if (!g_joined) join_cluster(); else printf("Already joined.\n"); break;
      case 'l': if (g_joined) { printf("Leaving...\n"); g_joined = false; }
                else printf("Not joined.\n"); break;
      case 's': show_status(); break;
      case 'q': g_running = false; break;
      default: printf("? (j/l/s/q)\n");
    }
  }

  socrates_shutdown(g_rt);
  printf("Goodbye.\n");
  return 0;
}
