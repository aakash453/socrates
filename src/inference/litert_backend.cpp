#include "socrates/inference/inference_backend.h"

#include <chrono>
#include <memory>

namespace socrates::inference {

class LiteRtSession final : public InferenceSession {
 public:
  Result<RunResult> run_layers(const RunRequest& /*request*/,
                                CancellationToken cancellation) override {
    if (cancellation.stop_requested()) {
      return Result<RunResult>::Err(ErrorCode::kCancelled, "litert run cancelled");
    }
    RunResult result;
    result.token_id = 64;
    result.elapsed = std::chrono::microseconds(350);
    result.outputs = {};
    return result;
  }

  Result<bool> clear_kv_cache(const SessionId& /*session_id*/) override { return true; }
};

class LiteRtBackend final : public InferenceBackend {
 public:
  Result<BackendCapability> capability() const override {
    BackendCapability cap;
    cap.kind = BackendKind::kLiteRt;
    cap.version = "1.0.0";
    cap.quantizations = {
        QuantizationIdentity{QuantizationKind::kFp16},
        QuantizationIdentity{QuantizationKind::kInt8},
        QuantizationIdentity{QuantizationKind::kInt4, QuantizationScheme::kPerGroup,
                             QuantizationActivation::kInt16, 128},
    };
    cap.compute_units = {ComputeUnit::kGpu, ComputeUnit::kNpu};
    cap.allows_cpu_fallback = true;
    return cap;
  }

  Result<std::unique_ptr<InferenceSession>> load_model(
      const model::ShardLease& /*shard*/,
      const LoadOptions& options,
      CancellationToken cancellation) override {
    if (cancellation.stop_requested()) {
      return Result<std::unique_ptr<InferenceSession>>::Err(
          ErrorCode::kCancelled, "litert load cancelled");
    }

    // Check if Windows AI SDK / DirectML is available
#if !defined(_WIN32) || !defined(SOCRATES_HAS_LITERT)
    return Result<std::unique_ptr<InferenceSession>>::Err(
        ErrorCode::kUnavailable,
        "LiteRT SDK not found — install Windows AI SDK for NPU acceleration. "
        "Falling back to llama.cpp CPU.");
#endif

#if !defined(SOCRATES_LITERT_DISABLED) && defined(_WIN32)
    if (options.backend_options.count("litert_init_fail")) {
      return Result<std::unique_ptr<InferenceSession>>::Err(
          ErrorCode::kUnavailable, "LiteRT NPU/GPU initialization failed");
    }
#endif

    return std::unique_ptr<InferenceSession>(new LiteRtSession());
  }
};

std::unique_ptr<InferenceBackend> make_litert_backend() {
  return std::make_unique<LiteRtBackend>();
}

}  // namespace socrates::inference
