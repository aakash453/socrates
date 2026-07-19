#include "socrates/inference/inference_backend.h"

#include <chrono>
#include <memory>

namespace socrates::inference {

class MlxSession final : public InferenceSession {
 public:
  Result<RunResult> run_layers(const RunRequest& /*request*/,
                                CancellationToken cancellation) override {
    if (cancellation.stop_requested()) {
      return Result<RunResult>::Err(ErrorCode::kCancelled, "mlx run cancelled");
    }
    RunResult result;
    result.token_id = 55;
    result.elapsed = std::chrono::microseconds(200);
    result.outputs = {};
    return result;
  }

  Result<bool> clear_kv_cache(const SessionId& /*session_id*/) override { return true; }
};

class MlxBackend final : public InferenceBackend {
 public:
  Result<BackendCapability> capability() const override {
    BackendCapability cap;
    cap.kind = BackendKind::kMlx;
    cap.version = "0.20.0";
    cap.quantizations = {
        QuantizationIdentity{QuantizationKind::kFp16},
        QuantizationIdentity{QuantizationKind::kInt4, QuantizationScheme::kPerGroup,
                             QuantizationActivation::kFp16, 128},
        QuantizationIdentity{QuantizationKind::kInt8, QuantizationScheme::kPerChannel,
                             QuantizationActivation::kFp16, 128},
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
          ErrorCode::kCancelled, "mlx load cancelled");
    }

#if !defined(SOCRATES_MLX_DISABLED) && defined(__APPLE__)
    if (options.backend_options.count("mlx_init_fail")) {
      return Result<std::unique_ptr<InferenceSession>>::Err(
          ErrorCode::kUnavailable, "MLX GPU/ANE initialization failed");
    }
#endif

    return std::unique_ptr<InferenceSession>(new MlxSession());
  }
};

std::unique_ptr<InferenceBackend> make_mlx_backend() {
  return std::make_unique<MlxBackend>();
}

}  // namespace socrates::inference
