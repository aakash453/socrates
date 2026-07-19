#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "socrates/result.h"
#include "socrates/scheduler/scheduler.h"

namespace socrates::persistence {

struct SavedAssignment {
  NodeId node_id;
  ModelId model_id;
  ShardId shard_id;
  std::string manifest_revision;
  QuantizationKind quantization{QuantizationKind::kFp16};
  BackendKind backend{BackendKind::kLlamaCpp};
};

struct TokenRecord {
  RequestId request_id;
  SessionId session_id;
  std::uint64_t position{0};
  std::int32_t token_id{0};
};

class AssignmentStore {
 public:
  virtual ~AssignmentStore() = default;

  /**
   * Atomically persists assignments from a committed plan.
   * Preconditions: plan is valid and fence is not older than persisted fence.
   * Postconditions: either all assignments and fence are durable or none are.
   * Throws: no operational exceptions; Result reports I/O and stale-fence errors.
   * Thread safety: writes are serialized; reads may run concurrently.
   * Side effects: writes local durable state; never persists KV values.
   */
  virtual Result<bool> save_plan(const scheduler::PipelinePlan& plan) = 0;

  /**
   * Loads the most recently committed plan for one model revision.
   * Preconditions: model/revision are non-empty.
   * Postconditions: result is advisory and includes its persisted fence; callers
   * revalidate membership, capabilities, artifacts, and plan expiry.
   * Throws: no operational exceptions.
   * Thread safety: safe concurrently with writes.
   * Side effects: reads local durable state.
   */
  [[nodiscard]] virtual Result<std::optional<scheduler::PipelinePlan>> load_plan(
      const ModelId& model_id,
      const std::string& manifest_revision) const = 0;

  /**
   * Appends one committed generated token for bounded recovery replay.
   * Preconditions: position is exactly next for request/session and token was
   * emitted only after all stage KV updates committed.
   * Postconditions: token is durably ordered or no append occurs.
   * Throws: no operational exceptions.
   * Thread safety: appends for one request are serialized.
   * Side effects: writes non-prompt token IDs subject to configured retention.
   */
  virtual Result<bool> append_token(const TokenRecord& token) = 0;

  /**
   * Loads an ordered token range used to rebuild replacement-stage KV state.
   * Preconditions: requested count is within configured recovery retention.
   * Postconditions: records are contiguous and ascending or kDataLoss is returned.
   * Throws: no operational exceptions.
   * Thread safety: safe concurrently with appends after a committed snapshot.
   * Side effects: reads local durable state.
   */
  [[nodiscard]] virtual Result<std::vector<TokenRecord>> load_token_history(
      const RequestId& request_id,
      std::uint64_t start_position,
      std::uint32_t maximum_count) const = 0;

  /**
   * Deletes retained request history after completion or expiration.
   * Preconditions: request has no active recovery operation.
   * Postconditions: its token records become unavailable; plans remain intact.
   * Throws: no operational exceptions.
   * Thread safety: serialized with append/load for the request.
   * Side effects: removes local recovery metadata, never backend KV memory.
   */
  virtual Result<bool> erase_token_history(const RequestId& request_id) = 0;

  /**
   * Looks up a previous assignment to accelerate safe rejoin.
   * Preconditions: identifiers are non-empty.
   * Postconditions: returned assignment is advisory and MUST be revalidated
   * against current manifest, capability, membership, and fence.
   * Throws: no operational exceptions.
   * Thread safety: safe concurrently with writes.
   * Side effects: reads local durable state.
   */
  [[nodiscard]] virtual Result<std::optional<SavedAssignment>> find(
      const NodeId& node_id,
      const ModelId& model_id,
      const std::string& manifest_revision) const = 0;
};

std::unique_ptr<AssignmentStore> make_sqlite_assignment_store(
    const std::string& db_path);

}  // namespace socrates::persistence
