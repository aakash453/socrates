#include "socrates/inference/inference_backend.h"

#include <chrono>
#include <memory>
#include <mutex>

namespace socrates::inference {

class ExecuTorchSession final : public InferenceSession {
 public:
  Result<RunResult> run_layers(const RunRequest& /*request*/,
                                CancellationToken cancellation) override {
    if (cancellation.stop_requested()) {
      return Result<RunResult>::Err(ErrorCode::kCancelled, "executorch run cancelled");
    }
    RunResult result;
    result.token_id = 99;
    result.elapsed = std::chrono::microseconds(300);
    result.outputs = {};
    return result;
  }

  Result<bool> clear_kv_cache(const SessionId& /*session_id*/) override { return true; }
};

class ExecuTorchQnnBackend final : public InferenceBackend {
 public:
  explicit ExecuTorchQnnBackend(bool cpu_fallback)
      : is_cpu_fallback_(cpu_fallback) {}

  Result<BackendCapability> capability() const override {
    BackendCapability cap;
    cap.kind = is_cpu_fallback_ ? BackendKind::kExecuTorchCpu
                                 : BackendKind::kExecuTorchQnn;
    cap.version = "0.4.0";
    cap.quantizations = {
        QuantizationIdentity{QuantizationKind::kFp16},
        QuantizationIdentity{QuantizationKind::kInt8},
    };
    cap.compute_units = is_cpu_fallback_ ? std::vector<ComputeUnit>{ComputeUnit::kCpu}
                                          : std::vector<ComputeUnit>{ComputeUnit::kNpu};
    cap.allows_cpu_fallback = !is_cpu_fallback_;
    return cap;
  }

  Result<std::unique_ptr<InferenceSession>> load_model(
      const model::ShardLease& /*shard*/,
      const LoadOptions& options,
      CancellationToken cancellation) override {
    if (cancellation.stop_requested()) {
      return Result<std::unique_ptr<InferenceSession>>::Err(
          ErrorCode::kCancelled, "executorch load cancelled");
    }

    // Check if QNN SDK is available
#if !defined(SOCRATES_HAS_QNN)
    if (!is_cpu_fallback_) {
      return Result<std::unique_ptr<InferenceSession>>::Err(
          ErrorCode::kUnavailable,
          "QNN SDK not found — install Qualcomm QNN SDK for Hexagon NPU. "
          "Falling back to CPU.");
    }
#endif

    if (!is_cpu_fallback_ && options.backend_options.count("qnn_init_fail")) {
      return Result<std::unique_ptr<InferenceSession>>::Err(
          ErrorCode::kUnavailable, "QNN initialization failed");
    }

    return std::unique_ptr<InferenceSession>(new ExecuTorchSession());
  }

 private:
  bool is_cpu_fallback_;
};

std::unique_ptr<InferenceBackend> make_executorch_qnn_backend() {
  return std::make_unique<ExecuTorchQnnBackend>(false);
}

std::unique_ptr<InferenceBackend> make_executorch_cpu_backend() {
  return std::make_unique<ExecuTorchQnnBackend>(true);
}

}  // namespace socrates::inference
