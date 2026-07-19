// bindings/c/socrates_c.h
// Stable C ABI for Socrates Runtime — consumed by JNI, ObjC++, C++/WinRT, and daemons.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOCRATES_API_VERSION 2

typedef void* socrates_runtime_t;
typedef void* socrates_handle_t;

typedef struct {
  int32_t code;
  const char* message;
} socrates_error_t;

typedef struct {
  const char* request_id;
  int32_t kind;
  uint64_t sequence;
  const char* text_delta;
  int32_t token_id;
} socrates_stream_event_t;

typedef void (*socrates_stream_callback_t)(const socrates_stream_event_t* event, void* user_data);

typedef struct {
  uint32_t version;
  const char* state_directory;
  const char* model_root;
  const char* cluster_id;
  int32_t trust_mode;
  uint32_t recovery_token_retention;
  bool tracing_enabled;
  bool start_as_leech;
  bool production_mode;
  int32_t batching_mode;
  uint32_t max_batch_size;
  uint32_t batch_timeout_ms;
  uint32_t long_context_threshold;
  bool skip_discovery;
} socrates_config_t;

socrates_error_t socrates_init(socrates_config_t config, socrates_runtime_t* out_runtime);
socrates_error_t socrates_start(socrates_runtime_t runtime);
socrates_error_t socrates_stop(socrates_runtime_t runtime);
socrates_error_t socrates_shutdown(socrates_runtime_t runtime);
const char* socrates_version(void);

socrates_error_t socrates_generate(socrates_runtime_t runtime,
    const char* request_id, const char* prompt, uint32_t max_tokens,
    float temperature, uint32_t context_window,
    socrates_stream_callback_t callback, void* user_data,
    socrates_handle_t* out_handle);

socrates_error_t socrates_cancel(socrates_handle_t handle);
socrates_error_t socrates_cancel_by_id(socrates_runtime_t runtime, const char* generation_id);
socrates_error_t socrates_join_cluster(socrates_runtime_t runtime);
char* socrates_model_progress_json(socrates_runtime_t runtime);
char* socrates_available_models_json(socrates_runtime_t runtime);
char* socrates_cluster_snapshot_json(socrates_runtime_t runtime);
char* socrates_run_profiler_json(socrates_runtime_t runtime);
char* socrates_estimate_context_json(socrates_runtime_t runtime, const char* model_id, uint32_t batch_size);

#ifdef __cplusplus
}
#endif
