// include/socrates/types.h
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace socrates {

// ── Primitives ───────────────────────────────────────────────────────────────

using Bytes = std::vector<std::byte>;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
using Attributes = std::map<std::string, std::string>;

using Sha256Digest = std::array<std::byte, 32>;

struct FencingToken {
  std::string value;
};

// ── Strong IDs ───────────────────────────────────────────────────────────────

#define SOCRATES_STRONG_ID(Name)                          \
  struct Name {                                         \
    std::string value;                                  \
    [[nodiscard]] bool empty() const {                  \
      return value.empty();                             \
    }                                                   \
    [[nodiscard]] bool operator==(const Name& o) const {\
      return value == o.value;                          \
    }                                                   \
    [[nodiscard]] bool operator!=(const Name& o) const {\
      return value != o.value;                          \
    }                                                   \
    [[nodiscard]] bool operator<(const Name& o) const { \
      return value < o.value;                           \
    }                                                   \
    [[nodiscard]] std::string_view sv() const {         \
      return value;                                     \
    }                                                   \
  }

SOCRATES_STRONG_ID(NodeId);
SOCRATES_STRONG_ID(RequestId);
SOCRATES_STRONG_ID(SessionId);
SOCRATES_STRONG_ID(ModelId);
SOCRATES_STRONG_ID(ShardId);
SOCRATES_STRONG_ID(TraceId);
SOCRATES_STRONG_ID(ManifestId);
SOCRATES_STRONG_ID(PlanId);
SOCRATES_STRONG_ID(ProfileId);

#undef SOCRATES_STRONG_ID

// ── Enums ────────────────────────────────────────────────────────────────────

enum class TensorLayout : std::uint8_t { kRowMajor, kColumnMajor };

enum class QuantizationKind : std::uint8_t {
  kFp16,
  kInt8,
  kInt4,
  kInt2,
  kTernary,
};

enum class QuantizationScheme : std::uint8_t {
  kPerTensor,
  kPerChannel,
  kPerGroup,
};

enum class QuantizationActivation : std::uint8_t {
  kFp8,
  kFp16,
  kInt16,
  kInt32,
};

enum class BackendKind : std::uint8_t {
  kLlamaCpp,
  kExecuTorchQnn,
  kExecuTorchCpu,
  kMlx,
  kLiteRt,
};

enum class ComputeUnit : std::uint8_t { kCpu, kGpu, kNpu };

enum class ElementType : std::uint8_t {
  kInt8,
  kInt16,
  kInt32,
  kInt64,
  kFloat16,
  kBf16,
  kFloat32,
  kFloat64,
};

enum class NodeState : std::uint8_t {
  kUnknown,
  kJoining,
  kLive,
  kSuspect,
  kLeft,
};

enum class RuntimeState : std::uint8_t {
  kStarting,
  kDiscovering,
  kJoining,
  kElecting,
  kReady,
  kReplanning,
  kDegraded,
  kStopping,
  kStopped,
};

// ── Value types ──────────────────────────────────────────────────────────────

struct LayerRange {
  std::uint32_t start{0};
  std::uint32_t end_exclusive{0};

  [[nodiscard]] bool valid() const { return end_exclusive > start; }
  [[nodiscard]] std::uint32_t count() const { return end_exclusive - start; }
};

struct QuantizationIdentity {
  QuantizationKind kind{QuantizationKind::kFp16};
  QuantizationScheme scheme{QuantizationScheme::kPerTensor};
  QuantizationActivation activation{QuantizationActivation::kFp16};
  std::uint32_t group_size{0};
  bool has_act_order{false};
  bool has_gptq{false};
};

struct TensorShape {
  std::vector<std::int64_t> dims;
  std::optional<std::int64_t> dynamic_min;
  std::optional<std::int64_t> dynamic_max;
  std::int32_t alignment{0};
};

struct TensorSpec {
  ElementType element_type{ElementType::kFloat32};
  TensorLayout layout{TensorLayout::kRowMajor};
  TensorShape shape;
  std::uint64_t maximum_encoded_bytes{0};
};

struct Tensor {
  TensorSpec spec;
  Bytes data;
};

struct NetworkMetrics {
  std::chrono::microseconds round_trip_latency{0};
  std::uint64_t available_bandwidth_bytes_per_second{0};
  std::chrono::system_clock::time_point measured_at;

  [[nodiscard]] bool valid() const { return round_trip_latency.count() > 0; }
};

struct BackendCapability {
  BackendKind kind{BackendKind::kLlamaCpp};
  std::string version;
  std::vector<QuantizationIdentity> quantizations;
  std::vector<ComputeUnit> compute_units;
  bool allows_cpu_fallback{false};
};

struct BackendBenchmark {
  BackendKind backend{BackendKind::kLlamaCpp};
  QuantizationIdentity quantization{};
  std::uint32_t sequence_length{0};
  double prefill_tokens_per_second{0.0};
  double decode_tokens_per_second{0.0};
  std::chrono::microseconds first_token_latency{0};
  std::uint64_t peak_memory_bytes{0};
};

struct CapabilityProfile {
  NodeId node_id;
  std::uint64_t revision{0};
  std::chrono::system_clock::time_point measured_at;
  Clock::time_point received_at{Clock::now()};
  std::chrono::milliseconds valid_for{0};
  std::uint64_t total_memory_bytes{0};
  std::uint64_t available_memory_bytes{0};
  std::string cpu_model;
  std::uint32_t logical_cpu_count{0};
  std::vector<std::string> accelerators;
  std::vector<BackendCapability> backends;
  std::vector<BackendBenchmark> measured_benchmarks;
  NetworkMetrics network_to_leader;
  Attributes extensions;

  [[nodiscard]] bool expired() const {
    return Clock::now() >= received_at + valid_for;
  }
};

struct LeadershipFence {
  std::uint64_t term{0};
  FencingToken token;
  std::uint64_t membership_revision{0};

  [[nodiscard]] bool newer_than(const LeadershipFence& other) const {
    return term > other.term;
  }

  [[nodiscard]] bool operator==(const LeadershipFence& o) const {
    return term == o.term && token.value == o.token.value &&
           membership_revision == o.membership_revision;
  }

  [[nodiscard]] bool operator!=(const LeadershipFence& o) const {
    return !(*this == o);
  }
};

// StageAssignment and PipelinePlan are defined in socrates/scheduler/scheduler.h
// (authoritative types consumed by runtime, pipeline, and persistence).

}  // namespace socrates
