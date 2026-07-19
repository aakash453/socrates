// include/socrates/model/model_manager.h
#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "socrates/result.h"
#include "socrates/scheduler/scheduler.h"
#include "socrates/types.h"

namespace socrates::model {

class ShardLease {
 public:
  virtual ~ShardLease() = default;
  [[nodiscard]] virtual const ShardId& shard_id() const noexcept = 0;
  [[nodiscard]] virtual const std::filesystem::path& path() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;
};

class ModelManager {
 public:
  virtual ~ModelManager() = default;

  virtual Result<std::unique_ptr<ShardLease>> acquire(
      const scheduler::StageAssignment& assignment,
      bool cancelled) = 0;

  virtual Result<bool> prune(std::uint64_t target_free_bytes) = 0;
};

std::unique_ptr<ModelManager> make_model_manager(
    const std::filesystem::path& model_root);

}  // namespace socrates::model
