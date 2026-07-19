// include/socrates/model/manifest_repository.h
#pragma once

#include <filesystem>
#include <string>

#include "socrates/result.h"
#include "socrates/scheduler/scheduler.h"

namespace socrates::model {

struct ManifestSummary {
  ModelId model_id;
  std::string revision;
  std::uint32_t total_layers{0};
  std::vector<scheduler::ShardOption> shard_options;
  std::vector<scheduler::BoundaryContract> boundaries;
};

class ManifestRepository {
 public:
  virtual ~ManifestRepository() = default;

  /**
   * Parses, checksum-verifies, semantically validates, and indexes a manifest.
   * Preconditions: path is a readable regular file containing a v1 envelope.
   * Postconditions: success atomically publishes immutable identity/revision;
   * conflicting bytes for an existing identity/revision are rejected.
   * Throws: no operational exceptions; Result reports I/O, parse, checksum,
   * unknown-field, and semantic validation failures.
   * Thread safety: concurrent reads and publication of distinct revisions are
   * supported; publication of the same key is serialized.
   * Side effects: reads the file and updates the local manifest index.
   */
  virtual Result<ManifestSummary> publish(
      const std::filesystem::path& path) = 0;

  /**
   * Resolves an already published manifest summary.
   * Preconditions: identifiers are non-empty.
   * Postconditions: returned value is a stable immutable copy.
   * Throws: no operational exceptions; Result returns kNotFound if absent.
   * Thread safety: safe concurrently with publish().
   * Side effects: none.
   */
  [[nodiscard]] virtual Result<ManifestSummary> resolve(
      const ModelId& model_id,
      const std::string& revision) const = 0;
};

std::unique_ptr<ManifestRepository> make_manifest_repository();

}  // namespace socrates::model
