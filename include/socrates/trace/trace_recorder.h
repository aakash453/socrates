#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "socrates/result.h"
#include "socrates/types.h"

namespace socrates::trace {

enum class TraceEventKind {
  kScheduleDecision,
  kLayerStarted,
  kLayerFinished,
  kTransferStarted,
  kTransferFinished,
  kTokenGenerated,
  kFailure,
};

struct TraceEvent {
  TraceId trace_id;
  RequestId request_id;
  TraceEventKind kind{TraceEventKind::kScheduleDecision};
  std::uint64_t sequence{0};
  std::chrono::system_clock::time_point wall_time;
  std::chrono::nanoseconds monotonic_offset{0};
  NodeId node_id;
  Attributes attributes;
};

class TraceRecorder {
 public:
  virtual ~TraceRecorder() = default;

  /**
   * Enables or disables future trace admission.
   * Preconditions: none.
   * Postconditions: disabling prevents new traces but does not corrupt active ones.
   * Throws: never.
   * Thread safety: atomic and concurrently callable.
   * Side effects: changes tracing overhead for future requests.
   */
  virtual void set_enabled(bool enabled) noexcept = 0;

  /**
   * Begins one request trace and reserves its bounded event buffer.
   * Preconditions: trace/request IDs are non-empty and trace is not active.
   * Postconditions: success admits record() calls for the trace.
   * Throws: no operational exceptions.
   * Thread safety: safe concurrently for distinct trace IDs.
   * Side effects: allocates bounded trace storage.
   */
  virtual Result<bool> begin(const TraceId& trace_id,
                             const RequestId& request_id) = 0;

  /**
   * Marks an active trace complete or aborted with a terminal status.
   * Preconditions: trace was begun and has not ended.
   * Postconditions: buffered events are sealed and exportable exactly once per
   * destination; later record() calls are rejected.
   * Throws: no operational exceptions.
   * Thread safety: serialized with record() for the same trace.
   * Side effects: flushes trace metadata to durable storage when configured.
   */
  virtual Result<bool> end(
      const TraceId& trace_id,
      const std::optional<RuntimeError>& terminal_error) = 0;

  /**
   * Appends one metadata-only event.
   * Preconditions: sequence is monotonic per trace and attributes contain no
   * prompts, tensor values, model bytes, credentials, or private keys.
   * Postconditions: enabled traces retain the event or report bounded-buffer
   * exhaustion; disabled traces return success without storage.
   * Throws: no operational exceptions.
   * Thread safety: safe from all runtime worker threads.
   * Side effects: writes to an in-memory/durable trace buffer.
   */
  virtual Result<bool> record(const TraceEvent& event) = 0;

  /**
   * Exports one completed trace to a caller-selected file.
   * Preconditions: trace exists and destination parent is writable.
   * Postconditions: success atomically replaces destination with a complete
   * versioned trace; partial files are removed.
   * Throws: no operational exceptions.
   * Thread safety: safe concurrently with other traces; active trace export is
   * rejected with kFailedPrecondition.
   * Side effects: writes a local file.
   */
  virtual Result<std::filesystem::path> export_trace(
      const TraceId& trace_id,
      const std::filesystem::path& destination) = 0;
};

}  // namespace socrates::trace
