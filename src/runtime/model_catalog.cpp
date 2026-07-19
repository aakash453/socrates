// src/runtime/model_catalog.cpp
#include "socrates/runtime/model_catalog.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "nlohmann/json.hpp"

namespace socrates::runtime {

using json = nlohmann::json;

namespace {

double total_ram_gb(const cluster::MembershipSnapshot& snap) {
  double ram = 0;
  for (const auto& m : snap.members) {
    if (m.state != cluster::MemberState::kAlive) continue;
    if (m.role != cluster::NodeRole::kParticipant) continue;
    if (m.capability.has_value()) {
      ram += static_cast<double>(m.capability->total_memory_bytes) / (1024.0 * 1024.0 * 1024.0);
    }
  }
  return ram;
}

std::uint32_t participant_count(const cluster::MembershipSnapshot& snap) {
  std::uint32_t count = 0;
  for (const auto& m : snap.members) {
    if (m.state == cluster::MemberState::kAlive &&
        m.role == cluster::NodeRole::kParticipant) {
      ++count;
    }
  }
  return count;
}

}  // namespace

std::vector<ModelCatalogEntry> load_model_catalog(const std::string& manifest_path) {
  std::vector<ModelCatalogEntry> catalog;
  std::ifstream f(manifest_path);
  if (!f.is_open()) return catalog;

  json j;
  try {
    f >> j;
  } catch (...) {
    return catalog;
  }

  for (const auto& mj : j["models"]) {
    ModelCatalogEntry entry;
    entry.model_id = ModelId{mj["model_id"].get<std::string>()};
    entry.family = mj.value("family", "");
    entry.display_name = mj.value("display_name", "");
    entry.total_params = mj.value("total_params", "");
    entry.total_layers = mj.value("total_layers", 0u);
    entry.size_gb_int4 = mj.value("size_gb_int4", 0.0);
    entry.size_gb_fp16 = mj.value("size_gb_fp16", 0.0);
    entry.context_window_max = mj.value("context_window_max", 2048u);
    entry.default_context = mj.value("default_context", 2048u);

    std::string tier = mj.value("tier", "");
    if (tier == "comfortable") entry.tier = ModelTier::kComfortable;
    else if (tier == "fair") entry.tier = ModelTier::kFair;
    else if (tier == "medium") entry.tier = ModelTier::kMedium;
    else if (tier == "tight") entry.tier = ModelTier::kTight;
    else if (tier == "debug") entry.tier = ModelTier::kDebug;

    if (mj.contains("profiling") && mj["profiling"].value("enabled", false)) {
      entry.is_profiler = true;
    }

    // Compatible backends from first shard
    if (!mj["shards"].empty()) {
      for (const auto& b : mj["shards"][0]["compatible_backends"]) {
        entry.compatible_backends.push_back(b.get<std::string>());
      }
    }

    // Pipeline options
    for (auto& [name, pj] : mj["pipelines"].items()) {
      PipelineOption opt;
      opt.name = name;
      opt.description = pj.value("description", "");
      opt.min_devices = pj.value("min_devices", 1u);
      opt.min_total_ram_gb = pj.value("min_total_ram_gb", 4u);
      for (const auto& sj : pj["stages"]) {
        auto layers = sj["layers"];
        opt.stage_layers.push_back(
            layers[1].get<std::uint32_t>() - layers[0].get<std::uint32_t>());
      }
      entry.pipelines.push_back(opt);
    }

    // Cluster configs that can run this
    if (j.contains("cluster_configs")) {
      for (const auto& cc : j["cluster_configs"]) {
        double cc_ram = cc.value("total_ram_gb", 0.0);
        std::uint32_t cc_devices = static_cast<std::uint32_t>(cc["devices"].size());
        bool can_run = false;
        for (const auto& po : entry.pipelines) {
          if (cc_ram >= po.min_total_ram_gb && cc_devices >= po.min_devices) {
            can_run = true;
            break;
          }
        }
        if (can_run) {
          entry.supported_cluster_configs.push_back(cc["name"].get<std::string>());
        }
      }
    }

    catalog.push_back(entry);
  }
  return catalog;
}

std::vector<ModelCatalogEntry> model_catalog_all() {
  // Try model-repo/ relative to cwd first, then /tmp path for app bundles
  auto entries = load_model_catalog("model-repo/manifest.json");
  if (!entries.empty()) return entries;
  entries = load_model_catalog("/tmp/socrates-gui/models/manifest.json");
  if (!entries.empty()) return entries;
  return load_model_catalog("/tmp/socrates-demo/models/manifest.json");
}

std::vector<ModelCatalogEntry> available_models(
    const cluster::MembershipSnapshot& membership) {
  auto all = model_catalog_all();
  std::vector<ModelCatalogEntry> available;

  double ram = total_ram_gb(membership);
  std::uint32_t devices = participant_count(membership);

  for (const auto& entry : all) {
    for (const auto& po : entry.pipelines) {
      if (ram >= static_cast<double>(po.min_total_ram_gb) &&
          devices >= po.min_devices) {
        available.push_back(entry);
        break;
      }
    }
  }
  return available;
}

ContextWindowEstimate estimate_context_window(
    const cluster::MembershipSnapshot& membership,
    const ModelCatalogEntry& model,
    const pipeline::BatchingConfig& batching) {

  ContextWindowEstimate est;
  double total_ram = total_ram_gb(membership);
  std::uint32_t devices = participant_count(membership);

  // Per-token KV cache memory (bytes per token per layer)
  // Llama-like: 2 * n_kv_heads * head_dim * sizeof(fp16)
  // Approximate: 2 * 8 * 128 * 2 = 4096 bytes per token per layer for GQA
  constexpr double kv_bytes_per_token_per_layer = 4096.0;

  // Find the best matching pipeline
  const PipelineOption* best_po = nullptr;
  for (const auto& po : model.pipelines) {
    if (total_ram >= static_cast<double>(po.min_total_ram_gb) &&
        devices >= po.min_devices) {
      if (!best_po || po.min_total_ram_gb > best_po->min_total_ram_gb) {
        best_po = &po;
      }
    }
  }

  if (!best_po) {
    est.max_tokens = 0;
    est.recommended_tokens = 0;
    est.limiting_factor = "insufficient_resources";
    est.detail = "Not enough devices or RAM to run this model.";
    return est;
  }

  // Calculate available VRAM per device for KV cache
  // Model weights consume size_gb_int4 GB, split across devices
  double weight_per_device = model.size_gb_int4 / static_cast<double>(devices);
  // Each device has total_ram / devices GB total
  double ram_per_device = total_ram / static_cast<double>(devices);
  // Assume 30% for OS overhead
  double usable_ram_per_device = ram_per_device * 0.7;
  double kv_ram_per_device = usable_ram_per_device - weight_per_device;
  if (kv_ram_per_device < 0) kv_ram_per_device = 0;

  // KV cache is per-sequence. Each device holds layers_per_device layers.
  double layers_per_device = static_cast<double>(model.total_layers) / static_cast<double>(devices);
  double kv_bytes_per_token = kv_bytes_per_token_per_layer * layers_per_device;

  // How many tokens fit in the KV cache?
  double kv_ram_bytes = kv_ram_per_device * 1024.0 * 1024.0 * 1024.0;
  std::uint32_t max_ctx = static_cast<std::uint32_t>(kv_ram_bytes / kv_bytes_per_token);

  // Batching multiplies KV cache usage
  std::uint32_t batch_multiplier;
  switch (batching.mode) {
    case pipeline::BatchingMode::kImmediate: batch_multiplier = 1; break;
    case pipeline::BatchingMode::kSerial:    batch_multiplier = 1; break;
    case pipeline::BatchingMode::kAdaptive:  batch_multiplier = batching.max_batch_size; break;
    default: batch_multiplier = 1;
  }
  max_ctx = max_ctx / std::max(1u, batch_multiplier);

  // Clamp to model's maximum
  if (max_ctx > model.context_window_max) max_ctx = model.context_window_max;

  est.max_tokens = max_ctx;
  est.recommended_tokens = std::min(max_ctx, model.default_context);

  std::ostringstream detail;
  detail << "Cluster: " << devices << " participants, "
         << std::fixed << std::setprecision(1) << total_ram << "GB RAM. "
         << "Model: " << model.display_name << " ("
         << model.size_gb_int4 << "GB INT4). "
         << "Pipeline: " << best_po->name << " ("
         << best_po->stage_layers.size() << " stages). "
         << "KV cache per device: " << std::fixed << std::setprecision(1)
         << kv_ram_per_device << "GB. "
         << "Batch multiplier: " << batch_multiplier << "x. ";
  est.detail = detail.str();

  if (max_ctx >= model.context_window_max) {
    est.limiting_factor = "model_max";
  } else if (kv_ram_per_device < 0.1) {
    est.limiting_factor = "vram";
  } else {
    est.limiting_factor = "vram";
  }

  return est;
}

}  // namespace socrates::runtime
