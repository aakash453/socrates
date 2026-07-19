// src/runtime/backend_dispatch.cpp
// Platform-optimized backend selection with automatic fallback chains.

#include "socrates/runtime/backend_dispatch.h"
#include "socrates/logging.h"

#include <memory>
#include <string>
#include <vector>

// Forward declarations for backend factory functions (defined in
// inference/*_backend.cpp)
namespace socrates::inference {
std::unique_ptr<InferenceBackend> make_mlx_backend();
std::unique_ptr<InferenceBackend> make_llama_backend();
std::unique_ptr<InferenceBackend> make_executorch_cpu_backend();
std::unique_ptr<InferenceBackend> make_executorch_qnn_backend();
}  // namespace socrates::inference

namespace socrates::runtime {

// ── Platform-optimized backend dispatch ───────────────────────────────────

std::vector<std::shared_ptr<inference::InferenceBackend>>
build_backend_fallback_chain(inference::BackendRegistry& registry) {
  std::vector<std::shared_ptr<inference::InferenceBackend>> chain;
  auto log = make_log_context("backend");

#if defined(__APPLE__) && defined(__arm64__)
  // Apple Silicon: MLX (GPU + ANE) → llama.cpp (CPU + AMX)
  {
    auto mlx = std::shared_ptr<inference::InferenceBackend>(
        inference::make_mlx_backend());
    registry.register_backend(BackendKind::kMlx, mlx);
    chain.push_back(mlx);
  }
  log.info("backend chain: MLX (Metal GPU + ANE) -> llama.cpp (CPU)");

#elif defined(__ANDROID__) && defined(__aarch64__)
  // Android ARM64: QNN (NPU) → llama.cpp (CPU + NEON)
  #if defined(SOCRATES_HAS_QNN)
    auto qnn = std::shared_ptr<inference::InferenceBackend>(
        inference::make_executorch_qnn_backend());
    registry.register_backend(BackendKind::kExecuTorchQnn, qnn);
    chain.push_back(qnn);
  #endif
  log.info("backend chain: QNN (Hexagon NPU) -> llama.cpp (CPU)");

#elif defined(_WIN32) && defined(_M_ARM64)
  // Windows ARM64 (Snapdragon X Elite): LiteRT → QNN → llama.cpp
  #if defined(SOCRATES_ENABLE_LITERT)
    extern std::unique_ptr<inference::InferenceBackend> make_litert_backend();
    auto litert = std::shared_ptr<inference::InferenceBackend>(make_litert_backend());
    registry.register_backend(BackendKind::kLiteRt, litert);
    chain.push_back(litert);
  #endif
  log.info("backend chain: LiteRT -> QNN (NPU) -> llama.cpp (CPU)");

#else
  log.info("backend chain: llama.cpp (CPU) -> ExecuTorch CPU");
#endif

  // Always add llama.cpp CPU as fallback
  {
    auto llama = std::shared_ptr<inference::InferenceBackend>(
        inference::make_llama_backend());
    registry.register_backend(BackendKind::kLlamaCpp, llama);
    chain.push_back(llama);
  }

  // ExecuTorch CPU as last-resort
  {
    auto exec_cpu = std::shared_ptr<inference::InferenceBackend>(
        inference::make_executorch_cpu_backend());
    registry.register_backend(BackendKind::kExecuTorchCpu, exec_cpu);
    chain.push_back(exec_cpu);
  }

  return chain;
}

Result<std::unique_ptr<inference::InferenceSession>>
load_model_with_fallback(
    const std::vector<std::shared_ptr<inference::InferenceBackend>>& chain,
    const model::ShardLease& shard,
    const inference::LoadOptions& options,
    CancellationToken cancellation) {

  auto log = make_log_context("backend");

  for (size_t i = 0; i < chain.size(); ++i) {
    auto cap = chain[i]->capability();
    if (cap.is_err()) continue;

    auto kind = cap.value().kind;
    log.debug("trying backend " + std::to_string(static_cast<int>(kind)));

    auto result = chain[i]->load_model(shard, options, cancellation);

    if (result.is_ok()) {
      log.info("model loaded on backend " + std::to_string(static_cast<int>(kind)));
      return result;
    }

    log.warn("backend " + std::to_string(static_cast<int>(kind)) + " failed, trying fallback");
  }

  return Result<std::unique_ptr<inference::InferenceSession>>::Err(
      ErrorCode::kUnavailable, "all backends exhausted");
}

}  // namespace socrates::runtime
