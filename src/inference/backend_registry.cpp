#include "socrates/inference/inference_backend.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace socrates::inference {

class BackendRegistryImpl final : public BackendRegistry {
 public:
  void register_backend(BackendKind kind, std::shared_ptr<InferenceBackend> backend) override {
    std::lock_guard lock(mutex_);
    backends_[kind] = std::move(backend);
  }

  Result<std::shared_ptr<InferenceBackend>> resolve(BackendKind kind) const override {
    std::lock_guard lock(mutex_);
    auto it = backends_.find(kind);
    if (it == backends_.end()) {
      return Result<std::shared_ptr<InferenceBackend>>::Err(
          ErrorCode::kNotFound, "backend not registered");
    }
    return it->second;
  }

  Result<std::vector<BackendKind>> fallback_chain(
      const FallbackRequest& request) const override {
    std::vector<BackendKind> chain;
    chain.push_back(request.requested);

    if (!request.allow_cpu_fallback) {
      return chain;
    }

    switch (request.requested) {
      case BackendKind::kExecuTorchQnn:
        chain.push_back(BackendKind::kExecuTorchCpu);
        break;

      case BackendKind::kMlx:
        chain.push_back(BackendKind::kExecuTorchCpu);
        break;

      case BackendKind::kLiteRt:
        chain.push_back(BackendKind::kExecuTorchCpu);
        break;

      case BackendKind::kLlamaCpp:
        chain.push_back(BackendKind::kExecuTorchCpu);
        break;

      case BackendKind::kExecuTorchCpu:
        break;
    }

    return chain;
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<BackendKind, std::shared_ptr<InferenceBackend>> backends_;
};

std::unique_ptr<BackendRegistry> make_backend_registry() {
  return std::make_unique<BackendRegistryImpl>();
}

}  // namespace socrates::inference
