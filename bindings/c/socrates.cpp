// bindings/c/socrates.cpp
// Stable C ABI implementation — translates between C types and socrates runtime.

#include "socrates_c.h"

#include "socrates/cancellation.h"
using socrates::CancellationToken;

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "socrates/pipeline/inference_pipeline.h"
#include "socrates/runtime/edge_runtime.h"
#include "socrates/runtime/model_catalog.h"
#include "socrates/cluster/membership_service.h"

namespace {

struct RuntimeState {
  std::unique_ptr<socrates::runtime::EdgeRuntime> runtime;
  socrates::runtime::RuntimeConfig config;
  std::unordered_map<std::string,
                     std::unique_ptr<socrates::pipeline::GenerationHandle>>
      handles;
  std::mutex mutex;
};

RuntimeState g_state;
std::mutex g_init_mutex;
bool g_initialized{false};

socrates_error_t make_error(int32_t code, const char* msg) {
  socrates_error_t err;
  err.code = code;
  err.message = msg;
  return err;
}

socrates_error_t make_ok() { return make_error(0, nullptr); }

char* strdup_c(const std::string& s) {
  char* p = static_cast<char*>(malloc(s.size() + 1));
  if (p) {
    std::memcpy(p, s.c_str(), s.size() + 1);
  }
  return p;
}

}  // namespace

socrates_error_t socrates_init(socrates_config_t config,
                               socrates_runtime_t* out_runtime) {
  std::lock_guard lock(g_init_mutex);

  if (g_initialized) {
    *out_runtime = &g_state;
    return make_ok();
  }

  if (!out_runtime) return make_error(1, "out_runtime is null");
  if (config.version < 1) return make_error(2, "unsupported config version");

  g_state.runtime = socrates::runtime::make_edge_runtime();

  if (config.state_directory && config.state_directory[0] != '\0') {
    g_state.config.state_directory = config.state_directory;
  } else {
    g_state.config.state_directory = "/tmp/socrates-state";
  }

  if (config.model_root && config.model_root[0] != '\0') {
    g_state.config.model_root = config.model_root;
  } else {
    g_state.config.model_root = "/tmp/socrates-models";
  }

  if (config.cluster_id && config.cluster_id[0] != '\0') {
    g_state.config.cluster_id = config.cluster_id;
  } else {
    g_state.config.cluster_id = "default-cluster";
  }

  g_state.config.local_node =
      socrates::NodeId{config.cluster_id ? std::string(config.cluster_id) +
                                               "-node"
                                         : "default-node"};

  switch (config.trust_mode) {
    case 1:
      g_state.config.identity.trust_mode =
          socrates::runtime::TrustMode::kPinnedAllowlist;
      break;
    case 2:
      g_state.config.identity.trust_mode =
          socrates::runtime::TrustMode::kPrivateCertificateAuthority;
      break;
    default:
      g_state.config.identity.trust_mode =
          socrates::runtime::TrustMode::kEphemeralLocalCluster;
      break;
  }

  g_state.config.recovery_token_retention = config.recovery_token_retention;
  g_state.config.tracing_enabled = config.tracing_enabled;
  g_state.config.start_as_leech = config.start_as_leech;
  g_state.config.production_mode = config.production_mode;
  g_state.config.skip_discovery = config.skip_discovery;

  // Batching config
  g_state.config.batching.mode = static_cast<socrates::pipeline::BatchingMode>(config.batching_mode);
  g_state.config.batching.max_batch_size = config.max_batch_size;
  g_state.config.batching.batch_timeout = std::chrono::milliseconds(config.batch_timeout_ms);
  g_state.config.batching.long_context_threshold = config.long_context_threshold;

  g_initialized = true;
  *out_runtime = &g_state;
  return make_ok();
}

socrates_error_t socrates_start(socrates_runtime_t runtime) {
  auto* state = static_cast<RuntimeState*>(runtime);
  if (!state || !state->runtime) return make_error(9, "runtime not initialized");

  auto result = state->runtime->start(
      state->config,
      [](const socrates::runtime::RuntimeSnapshot&) {},
      CancellationToken{});

  if (result.is_err()) {
    return make_error(static_cast<int32_t>(result.error_code()),
                      result.error().what());
  }
  return make_ok();
}

socrates_error_t socrates_generate(socrates_runtime_t runtime,
                                   const char* request_id,
                                   const char* prompt, uint32_t max_tokens,
                                   float temperature,
                                   uint32_t context_window,
                                   socrates_stream_callback_t callback,
                                   void* user_data,
                                   socrates_handle_t* out_handle) {
  auto* state = static_cast<RuntimeState*>(runtime);
  if (!state || !state->runtime) return make_error(9, "runtime not initialized");

  socrates::pipeline::InferenceRequest req;
  req.request_id = socrates::RequestId{request_id ? request_id : "req"};
  req.session_id = socrates::SessionId{"session-1"};
  req.model_id = socrates::ModelId{"default-model"};
  req.prompt = prompt ? prompt : "";
  req.generation.maximum_new_tokens = max_tokens;
  req.generation.temperature = temperature;
  req.generation.context_window = context_window;

  auto handler = [callback, user_data](
                     const socrates::pipeline::StreamEvent& ev) {
    if (!callback) return;
    socrates_stream_event_t c_ev;
    c_ev.request_id = ev.request_id.value.c_str();
    c_ev.kind = static_cast<int32_t>(ev.kind);
    c_ev.sequence = ev.sequence;
    c_ev.text_delta = ev.text_delta.c_str();
    c_ev.token_id = ev.token_id.value_or(-1);
    callback(&c_ev, user_data);
  };

  auto result = state->runtime->generate(req, handler, CancellationToken{});
  if (result.is_err()) {
    return make_error(static_cast<int32_t>(result.error_code()),
                      result.error().what());
  }

  std::lock_guard lock(state->mutex);
  std::string key = req.request_id.value;
  state->handles[key] = std::move(result.value());
  if (out_handle) *out_handle = state->handles[key].get();
  return make_ok();
}

socrates_error_t socrates_cancel(socrates_handle_t handle) {
  if (!handle) return make_error(2, "null handle");
  auto* gen = static_cast<socrates::pipeline::GenerationHandle*>(handle);
  gen->cancel();
  return make_ok();
}

socrates_error_t socrates_cancel_by_id(socrates_runtime_t runtime,
                                       const char* generation_id) {
  auto* state = static_cast<RuntimeState*>(runtime);
  if (!state) return make_error(9, "runtime not initialized");
  std::lock_guard lock(state->mutex);
  auto it = state->handles.find(generation_id ? generation_id : "");
  if (it == state->handles.end()) return make_error(3, "not found");
  it->second->cancel();
  state->handles.erase(it);
  return make_ok();
}

socrates_error_t socrates_stop(socrates_runtime_t runtime) {
  auto* state = static_cast<RuntimeState*>(runtime);
  if (!state || !state->runtime) return make_error(9, "not initialized");
  state->runtime->stop();
  return make_ok();
}

socrates_error_t socrates_shutdown(socrates_runtime_t runtime) {
  auto* state = static_cast<RuntimeState*>(runtime);
  if (!state || !state->runtime) return make_error(9, "not initialized");
  state->runtime->stop();
  state->runtime.reset();
  { std::lock_guard lock(state->mutex); state->handles.clear(); }
  { std::lock_guard lock(g_init_mutex); g_initialized = false; }
  return make_ok();
}

const char* socrates_version(void) { return "0.2.0"; }

// ── v2 APIs ────────────────────────────────────────────────────────────────

socrates_error_t socrates_join_cluster(socrates_runtime_t runtime) {
  auto* state = static_cast<RuntimeState*>(runtime);
  if (!state || !state->runtime) return make_error(9, "not initialized");
  auto result = state->runtime->join_cluster(CancellationToken{});
  if (result.is_err()) {
    return make_error(static_cast<int32_t>(result.error_code()),
                      result.error().what());
  }
  return make_ok();
}

char* socrates_model_progress_json(socrates_runtime_t runtime) {
  auto* state = static_cast<RuntimeState*>(runtime);
  if (!state || !state->runtime) return strdup_c("[]");
  auto models = state->runtime->model_download_progress();
  std::ostringstream os;
  os << "[";
  for (size_t i = 0; i < models.size(); ++i) {
    if (i > 0) os << ",";
    os << "{\"model_id\":\"" << models[i].model_id.value << "\""
       << ",\"ready\":" << (models[i].ready ? "true" : "false")
       << ",\"downloaded\":" << models[i].downloaded_bytes
       << ",\"total\":" << models[i].total_bytes << "}";
  }
  os << "]";
  return strdup_c(os.str());
}

char* socrates_available_models_json(socrates_runtime_t runtime) {
  auto* state = static_cast<RuntimeState*>(runtime);
  if (!state || !state->runtime) return strdup_c("[]");
  auto models = state->runtime->available_models();
  std::ostringstream os;
  os << "[";
  for (size_t i = 0; i < models.size(); ++i) {
    if (i > 0) os << ",";
    os << "{\"model_id\":\"" << models[i].model_id.value << "\""
       << ",\"display_name\":\"" << models[i].display_name << "\""
       << ",\"family\":\"" << models[i].family << "\""
       << ",\"params\":\"" << models[i].total_params << "\""
       << ",\"size_gb\":" << models[i].size_gb_int4
       << ",\"context_max\":" << models[i].context_window_max
       << ",\"default_context\":" << models[i].default_context
       << ",\"tier\":" << static_cast<int>(models[i].tier)
       << ",\"is_profiler\":" << (models[i].is_profiler ? "true" : "false")
       << "}";
  }
  os << "]";
  return strdup_c(os.str());
}

char* socrates_cluster_snapshot_json(socrates_runtime_t runtime) {
  auto* state = static_cast<RuntimeState*>(runtime);
  if (!state || !state->runtime) return strdup_c("{}");
  auto snap_result = state->runtime->snapshot();
  if (snap_result.is_err()) return strdup_c("{}");
  auto& snap = snap_result.value();
  std::ostringstream os;
  os << "{";
  os << "\"revision\":\"" << snap.membership.revision << "\"";
  os << ",\"state\":" << static_cast<int>(snap.state);
  os << ",\"local_role\":" << static_cast<int>(snap.local_role);
  if (snap.leadership.leader_id.has_value()) {
    os << ",\"leader_id\":\"" << snap.leadership.leader_id->value << "\"";
    os << ",\"term\":" << snap.leadership.fence.term;
  }
  os << ",\"devices\":[";
  bool first = true;
  for (const auto& m : snap.membership.members) {
    if (!first) os << ","; first = false;
    os << "{\"node_id\":\"" << m.peer.node_id.value << "\""
       << ",\"state\":" << static_cast<int>(m.state)
       << ",\"role\":" << static_cast<int>(m.role);
    if (m.capability.has_value()) {
      os << ",\"ram_mb\":" << (m.capability->total_memory_bytes / (1024*1024))
         << ",\"cpu\":\"" << m.capability->cpu_model << "\""
         << ",\"cores\":" << m.capability->logical_cpu_count;
    }
    os << ",\"models_ready\":" << (m.models.empty() ? 0 : 
         std::count_if(m.models.begin(), m.models.end(),
                       [](auto& mp) { return mp.ready; }))
       << "}";
  }
  os << "]";
  if (snap.active_plan.has_value()) {
    os << ",\"stages\":[";
    for (size_t i = 0; i < snap.active_plan->stages.size(); ++i) {
      if (i > 0) os << ",";
      auto& s = snap.active_plan->stages[i];
      os << "{\"ordinal\":" << s.ordinal
         << ",\"node_id\":\"" << s.node_id.value << "\""
         << ",\"backend\":" << static_cast<int>(s.backend)
         << ",\"layers\":[" << (s.layers ? s.layers->start : 0)
         << "," << (s.layers ? s.layers->end_exclusive : 0) << "]}";
    }
    os << "]";
  }
  os << "}";
  return strdup_c(os.str());
}

char* socrates_run_profiler_json(socrates_runtime_t runtime) {
  auto* state = static_cast<RuntimeState*>(runtime);
  if (!state || !state->runtime) return strdup_c("{}");
  auto result = state->runtime->run_profiler(CancellationToken{});
  if (result.is_err()) return strdup_c("{}");
  auto& p = result.value();
  std::ostringstream os;
  os << "{\"healthy\":" << (p.all_stages_healthy ? "true" : "false")
     << ",\"tokens_per_sec\":" << p.total_tokens_per_second
     << ",\"latency_us\":" << p.end_to_end_latency.count()
     << ",\"stages\":[";
  for (size_t i = 0; i < p.stages.size(); ++i) {
    if (i > 0) os << ",";
    auto& s = p.stages[i];
    os << "{\"stage\":\"" << s.stage_id << "\""
       << ",\"node\":\"" << s.node_id.value << "\""
       << ",\"gpu_miss\":" << s.gpu_miss_rate
       << ",\"npu_miss\":" << s.npu_miss_rate
       << ",\"cpu_fallback\":" << s.cpu_fallback_rate
       << ",\"avg_latency_us\":" << s.avg_latency.count()
       << ",\"p99_latency_us\":" << s.p99_latency.count()
       << ",\"mem_mb\":" << (s.peak_memory_bytes / (1024*1024))
       << ",\"kv_hits\":" << s.kv_cache_hit_count
       << ",\"kv_misses\":" << s.kv_cache_miss_count
       << ",\"network_bytes\":" << s.network_transfer_bytes << "}";
  }
  os << "]}";
  return strdup_c(os.str());
}

char* socrates_estimate_context_json(socrates_runtime_t runtime,
                                      const char* model_id,
                                      uint32_t batch_size) {
  auto* state = static_cast<RuntimeState*>(runtime);
  if (!state || !state->runtime) return strdup_c("{}");
  // Get membership for the estimate
  auto snap = state->runtime->snapshot();
  if (snap.is_err()) return strdup_c("{}");

  // Find the model in the catalog
  auto all = socrates::runtime::model_catalog_all();
  const socrates::runtime::ModelCatalogEntry* model = nullptr;
  for (auto& m : all) {
    if (m.model_id.value == std::string(model_id ? model_id : "")) {
      model = &m;
      break;
    }
  }
  if (!model) return strdup_c("{\"error\":\"model not found\"}");

  socrates::pipeline::BatchingConfig bcfg;
  bcfg.mode = batch_size > 1 ? socrates::pipeline::BatchingMode::kAdaptive
                              : socrates::pipeline::BatchingMode::kImmediate;
  bcfg.max_batch_size = batch_size > 0 ? batch_size : 1;

  auto est = socrates::runtime::estimate_context_window(
      snap.value().membership, *model, bcfg);

  std::ostringstream os;
  os << "{\"max_tokens\":" << est.max_tokens
     << ",\"recommended\":" << est.recommended_tokens
     << ",\"limiting_factor\":\"" << est.limiting_factor << "\""
     << ",\"detail\":\"" << est.detail << "\""
     << ",\"model\":\"" << (model_id ? model_id : "") << "\""
     << ",\"batch\":" << batch_size << "}";
  return strdup_c(os.str());
}
