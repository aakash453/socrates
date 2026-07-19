// include/socrates/runtime/model_catalog.h
// Public API for browsing available models, estimating context windows,
// and running the debug profiler.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "socrates/pipeline/inference_pipeline.h"
#include "socrates/cluster/membership_service.h"
#include "socrates/types.h"

namespace socrates::runtime {

// ── Model catalog entry (displayed in UI) ───────────────────────────────

enum class ModelTier : std::uint8_t { kComfortable, kFair, kMedium, kTight, kDebug };

struct PipelineOption {
  std::string name;               // e.g. "fair_2node"
  std::string description;
  std::uint32_t min_devices;
  std::uint32_t min_total_ram_gb;
  std::vector<std::uint32_t> stage_layers;  // layer counts per stage
};

struct ModelCatalogEntry {
  ModelId model_id;
  std::string family;             // "qwen3", "gemma", "llama3", "debug"
  std::string display_name;       // "Qwen 3 4B"
  std::string total_params;       // "4B"
  std::uint32_t total_layers{0};
  double size_gb_int4;
  double size_gb_fp16;
  std::uint32_t context_window_max;
  std::uint32_t default_context;
  ModelTier tier;
  bool is_profiler{false};
  std::vector<std::string> compatible_backends;
  std::vector<PipelineOption> pipelines;
  // Which cluster configs can run this model
  std::vector<std::string> supported_cluster_configs;
};

// ── Per-stage profiling metrics ─────────────────────────────────────────

struct StageProfile {
  std::string stage_id;
  NodeId node_id;
  BackendKind backend;
  double gpu_miss_rate{0.0};       // fraction of ops that fell back to CPU
  double npu_miss_rate{0.0};
  double cpu_fallback_rate{0.0};
  std::chrono::microseconds avg_latency{0};
  std::chrono::microseconds p99_latency{0};
  std::uint64_t kv_cache_hit_count{0};
  std::uint64_t kv_cache_miss_count{0};
  std::uint64_t network_transfer_bytes{0};
  std::uint64_t peak_memory_bytes{0};
};

struct ClusterProfile {
  std::string cluster_config_name;
  ModelId profiler_model_id;
  std::vector<StageProfile> stages;
  double total_tokens_per_second{0.0};
  std::chrono::microseconds end_to_end_latency{0};
  bool all_stages_healthy{false};
};

// ── Context window estimation ───────────────────────────────────────────

struct ContextWindowEstimate {
  std::uint32_t max_tokens;
  std::uint32_t recommended_tokens;
  std::string limiting_factor;   // "vram", "network", "compute"
  std::string detail;            // human-readable breakdown
};

/// Estimate the maximum context window given cluster state, model, and batching.
ContextWindowEstimate estimate_context_window(
    const cluster::MembershipSnapshot& membership,
    const ModelCatalogEntry& model,
    const pipeline::BatchingConfig& batching);

// ── Model catalog access ────────────────────────────────────────────────

/// Returns all models that can run on the current cluster.
std::vector<ModelCatalogEntry> available_models(
    const cluster::MembershipSnapshot& membership);

/// Returns all model catalog entries (including those requiring more devices).
std::vector<ModelCatalogEntry> model_catalog_all();

/// Loads the model catalog from a manifest file.
std::vector<ModelCatalogEntry> load_model_catalog(const std::string& manifest_path);

}  // namespace socrates::runtime
