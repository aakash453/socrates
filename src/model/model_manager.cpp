#include "socrates/model/model_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace socrates::model {

class ShardLeaseImpl final : public ShardLease {
 public:
  ShardLeaseImpl(ShardId id, std::filesystem::path path, std::uint64_t size)
      : shard_id_(std::move(id)), path_(std::move(path)), size_bytes_(size) {}
  const ShardId& shard_id() const noexcept override { return shard_id_; }
  const std::filesystem::path& path() const noexcept override { return path_; }
  std::uint64_t size_bytes() const noexcept override { return size_bytes_; }

 private:
  ShardId shard_id_;
  std::filesystem::path path_;
  std::uint64_t size_bytes_;
};

class ModelManagerImpl final : public ModelManager {
 public:
  explicit ModelManagerImpl(std::filesystem::path model_root)
      : model_root_(std::move(model_root)) {}

  Result<std::unique_ptr<ShardLease>> acquire(
      const scheduler::StageAssignment& assignment,
      bool cancelled) override {
    if (cancelled) {
      return RuntimeError(ErrorCode::kCancelled, "acquisition cancelled");
    }

    std::string uri = assignment.artifact.uri;
    if (uri.find("file:./") != 0 && uri.find("file:") == 0) {
      return RuntimeError(ErrorCode::kInvalidArgument,
                          "only file:./ URIs supported: " + uri);
    }

    auto rel_start = uri.find("./");
    auto rel = (rel_start != std::string::npos) ? uri.substr(rel_start + 2) : uri;
    auto full = model_root_ / rel;

    {
      std::lock_guard lock(mutex_);
      if (active_leases_.count(assignment.shard_id.value)) {
        return RuntimeError(ErrorCode::kAlreadyExists, "shard already leased");
      }
    }

    std::error_code ec;
    if (!std::filesystem::exists(full, ec)) {
      return RuntimeError(ErrorCode::kNotFound,
                          "artifact not found: " + full.string());
    }

    auto sz = std::filesystem::file_size(full, ec);
    if (ec) {
      return RuntimeError(ErrorCode::kInternal, "failed to read artifact size");
    }

    auto lease = std::make_unique<ShardLeaseImpl>(assignment.shard_id, full, sz);
    active_leases_[assignment.shard_id.value] = full.string();
    return std::unique_ptr<ShardLease>(std::move(lease));
  }

  Result<bool> prune(std::uint64_t target_free_bytes) override {
    (void)target_free_bytes;
    return true;
  }

 private:
  std::filesystem::path model_root_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::string> active_leases_;
};

std::unique_ptr<ModelManager> make_model_manager(
    const std::filesystem::path& model_root) {
  return std::make_unique<ModelManagerImpl>(model_root);
}

}  // namespace socrates::model
