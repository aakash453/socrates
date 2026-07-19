#include "socrates/model/manifest_repository.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace socrates::model {

namespace {
std::string make_key(const ModelId& m, const std::string& rev) {
  return m.value + "/" + rev;
}

// ── Quantization kind resolver ────────────────────────────────────────────────

QuantizationKind resolve_quantization_kind(const std::string& kind_str) {
  if (kind_str == "fp16") return QuantizationKind::kFp16;
  if (kind_str == "int4") return QuantizationKind::kInt4;
  if (kind_str == "int8") return QuantizationKind::kInt8;
  if (kind_str == "int2") return QuantizationKind::kInt2;
  if (kind_str == "ternary") return QuantizationKind::kTernary;
  return QuantizationKind::kFp16;
}

// ── Stage kind resolver ───────────────────────────────────────────────────────

scheduler::StageKind resolve_stage_kind(std::string_view kind_str) {
  if (kind_str == "tokenEmbedding") return scheduler::StageKind::kTokenEmbedding;
  if (kind_str == "transformerLayers")
    return scheduler::StageKind::kTransformerLayers;
  if (kind_str == "outputNormalization")
    return scheduler::StageKind::kOutputNormalization;
  if (kind_str == "lmHead") return scheduler::StageKind::kLmHead;
  return scheduler::StageKind::kTransformerLayers;
}

// ── Backend kind resolver ─────────────────────────────────────────────────────

BackendKind resolve_backend_kind(std::string_view backend_str) {
  if (backend_str == "llamaCpp") return BackendKind::kLlamaCpp;
  if (backend_str == "execuTorchCpu") return BackendKind::kExecuTorchCpu;
  if (backend_str == "execuTorchQnn") return BackendKind::kExecuTorchQnn;
  if (backend_str == "mlx") return BackendKind::kMlx;
  if (backend_str == "liteRt") return BackendKind::kLiteRt;
  return BackendKind::kLlamaCpp;
}

// ── Compute unit resolver ─────────────────────────────────────────────────────

ComputeUnit resolve_compute_unit(std::string_view unit_str) {
  if (unit_str == "cpu") return ComputeUnit::kCpu;
  if (unit_str == "gpu") return ComputeUnit::kGpu;
  if (unit_str == "npu") return ComputeUnit::kNpu;
  return ComputeUnit::kCpu;
}

// ── Element type resolver ─────────────────────────────────────────────────────

ElementType resolve_element_type(std::string_view et_str) {
  if (et_str == "float16") return ElementType::kFloat16;
  if (et_str == "float32") return ElementType::kFloat32;
  if (et_str == "bf16") return ElementType::kBf16;
  if (et_str == "float64") return ElementType::kFloat64;
  if (et_str == "int8") return ElementType::kInt8;
  if (et_str == "int16") return ElementType::kInt16;
  if (et_str == "int32") return ElementType::kInt32;
  if (et_str == "int64") return ElementType::kInt64;
  return ElementType::kFloat32;
}

// ── Top-level JSON parser ─────────────────────────────────────────────────────

Result<ManifestSummary> parse_manifest_json(const nlohmann::json& j) {
  ManifestSummary summary;

  // Top-level identity fields
  if (!j.contains("modelId") || !j["modelId"].is_string()) {
    return Result<ManifestSummary>::Err(ErrorCode::kInvalidArgument,
                                        "manifest missing string field: modelId");
  }
  summary.model_id.value = j["modelId"].get<std::string>();

  if (!j.contains("version") || !j["version"].is_string()) {
    return Result<ManifestSummary>::Err(ErrorCode::kInvalidArgument,
                                        "manifest missing string field: version");
  }
  summary.revision = j["version"].get<std::string>();

  if (!j.contains("totalTransformerLayers") ||
      !j["totalTransformerLayers"].is_number_unsigned()) {
    return Result<ManifestSummary>::Err(
        ErrorCode::kInvalidArgument,
        "manifest missing unsigned integer field: totalTransformerLayers");
  }
  summary.total_layers = j["totalTransformerLayers"].get<std::uint32_t>();

  // Build quantization lookup map from top-level quantizations array
  std::unordered_map<std::string, QuantizationKind> quant_map;
  if (j.contains("quantizations") && j["quantizations"].is_array()) {
    for (const auto& q : j["quantizations"]) {
      if (!q.contains("id") || !q.contains("kind")) continue;
      std::string id = q["id"].get<std::string>();
      std::string kind = q["kind"].get<std::string>();
      quant_map[id] = resolve_quantization_kind(kind);
    }
  }

  // Parse shards array
  if (!j.contains("shards") || !j["shards"].is_array()) {
    return Result<ManifestSummary>::Err(ErrorCode::kInvalidArgument,
                                        "manifest missing array field: shards");
  }

  for (const auto& s : j["shards"]) {
    scheduler::ShardOption opt;

    // shardId
    if (!s.contains("shardId") || !s["shardId"].is_string()) continue;
    opt.shard_id.value = s["shardId"].get<std::string>();

    // stageIds
    if (s.contains("stageIds") && s["stageIds"].is_array()) {
      for (const auto& sid : s["stageIds"]) {
        opt.stage_ids.push_back(sid.get<std::string>());
      }
    }

    // stageKind
    if (s.contains("stageKind") && s["stageKind"].is_string()) {
      opt.stage_kind = resolve_stage_kind(s["stageKind"].get<std::string>());
    }

    // layers (optional)
    if (s.contains("layers") && s["layers"].is_object()) {
      const auto& l = s["layers"];
      if (l.contains("start") && l["start"].is_number_unsigned() &&
          l.contains("endExclusive") && l["endExclusive"].is_number_unsigned()) {
        opt.layers = LayerRange{l["start"].get<std::uint32_t>(),
                                l["endExclusive"].get<std::uint32_t>()};
      }
    }

    // quantizationId → resolve via lookup map
    if (s.contains("quantizationId") && s["quantizationId"].is_string()) {
      std::string qid = s["quantizationId"].get<std::string>();
      auto it = quant_map.find(qid);
      if (it != quant_map.end()) {
        opt.quantization = it->second;
      }
    }

    // artifact
    if (s.contains("artifact") && s["artifact"].is_object()) {
      const auto& a = s["artifact"];
      if (a.contains("uri")) opt.artifact.uri = a["uri"].get<std::string>();
      if (a.contains("sha256"))
        opt.artifact.sha256_hex = a["sha256"].get<std::string>();
      if (a.contains("format"))
        opt.artifact.format = a["format"].get<std::string>();
      if (a.contains("formatVersion"))
        opt.artifact.format_version = a["formatVersion"].get<std::string>();
      if (a.contains("fileBytes"))
        opt.artifact.file_bytes = a["fileBytes"].get<std::uint64_t>();
    }

    // executionProfileId
    if (s.contains("executionProfileId") &&
        s["executionProfileId"].is_string()) {
      opt.execution_profile_id = s["executionProfileId"].get<std::string>();
    }

    // peakRuntimeMemoryBytes
    if (s.contains("peakRuntimeMemoryBytes") &&
        s["peakRuntimeMemoryBytes"].is_number_unsigned()) {
      opt.peak_runtime_memory_bytes =
          s["peakRuntimeMemoryBytes"].get<std::uint64_t>();
    }

    // estimatedKvBytesPerToken
    if (s.contains("estimatedKvBytesPerToken") &&
        s["estimatedKvBytesPerToken"].is_number_unsigned()) {
      opt.estimated_kv_bytes_per_token =
          s["estimatedKvBytesPerToken"].get<std::uint64_t>();
    }

    // compatibleBackends
    if (s.contains("compatibleBackends") && s["compatibleBackends"].is_array()) {
      for (const auto& be : s["compatibleBackends"]) {
        opt.compatible_backends.push_back(
            resolve_backend_kind(be.get<std::string>()));
      }
    }

    // requiredComputeUnits
    if (s.contains("requiredComputeUnits") &&
        s["requiredComputeUnits"].is_array()) {
      for (const auto& cu : s["requiredComputeUnits"]) {
        opt.required_compute_units.push_back(
            resolve_compute_unit(cu.get<std::string>()));
      }
    }

    // requiredOperatorIds
    if (s.contains("requiredOperatorIds") &&
        s["requiredOperatorIds"].is_array()) {
      for (const auto& op : s["requiredOperatorIds"]) {
        opt.required_operator_ids.push_back(op.get<std::string>());
      }
    }

    // estimatedPrefillMicroseconds
    if (s.contains("estimatedPrefillMicroseconds") &&
        s["estimatedPrefillMicroseconds"].is_number()) {
      opt.estimated_prefill_microseconds =
          s["estimatedPrefillMicroseconds"].get<double>();
    }

    // estimatedDecodeMicroseconds
    if (s.contains("estimatedDecodeMicroseconds") &&
        s["estimatedDecodeMicroseconds"].is_number()) {
      opt.estimated_decode_microseconds =
          s["estimatedDecodeMicroseconds"].get<double>();
    }

    // sensitivityScore
    if (s.contains("sensitivityScore") &&
        s["sensitivityScore"].is_number()) {
      opt.sensitivity_score = s["sensitivityScore"].get<double>();
    }

    summary.shard_options.push_back(std::move(opt));
  }

  // Parse boundaries array
  if (j.contains("boundaries") && j["boundaries"].is_array()) {
    for (const auto& b : j["boundaries"]) {
      scheduler::BoundaryContract bc;

      if (b.contains("boundaryId") && b["boundaryId"].is_string()) {
        bc.boundary_id = b["boundaryId"].get<std::string>();
      }
      if (b.contains("producerStageId") && b["producerStageId"].is_string()) {
        bc.producer_stage_id = b["producerStageId"].get<std::string>();
      }
      if (b.contains("consumerStageId") && b["consumerStageId"].is_string()) {
        bc.consumer_stage_id = b["consumerStageId"].get<std::string>();
      }

      // tensors array
      if (b.contains("tensors") && b["tensors"].is_array()) {
        for (const auto& t : b["tensors"]) {
          scheduler::BoundaryTensorContract tensor;

          if (t.contains("tensorId") && t["tensorId"].is_string()) {
            tensor.tensor_id = t["tensorId"].get<std::string>();
          }
          if (t.contains("elementType") && t["elementType"].is_string()) {
            tensor.element_type =
                resolve_element_type(t["elementType"].get<std::string>());
          }
          if (t.contains("maximumShape") && t["maximumShape"].is_array()) {
            for (const auto& dim : t["maximumShape"]) {
              tensor.maximum_shape.push_back(dim.get<std::int64_t>());
            }
          }
          if (t.contains("layout") && t["layout"].is_string()) {
            tensor.layout = t["layout"].get<std::string>();
          }
          if (t.contains("maximumEncodedBytes") &&
              t["maximumEncodedBytes"].is_number_unsigned()) {
            tensor.maximum_encoded_bytes =
                t["maximumEncodedBytes"].get<std::uint64_t>();
          }

          bc.tensors.push_back(std::move(tensor));
        }
      }

      if (b.contains("transferable") && b["transferable"].is_boolean()) {
        bc.transferable = b["transferable"].get<bool>();
      }
      if (b.contains("reliableOrderedRequired") &&
          b["reliableOrderedRequired"].is_boolean()) {
        bc.reliable_ordered_required =
            b["reliableOrderedRequired"].get<bool>();
      }

      summary.boundaries.push_back(std::move(bc));
    }
  }

  return Result<ManifestSummary>::Ok(summary);
}

}  // namespace

// ── ManifestRepository implementation ─────────────────────────────────────────

class ManifestRepositoryImpl final : public ManifestRepository {
 public:
  Result<ManifestSummary> publish(
      const std::filesystem::path& path) override {
    std::lock_guard lock(mutex_);

    // 1. Read the file
    std::ifstream f(path, std::ios::binary);
    if (!f) {
      return Result<ManifestSummary>::Err(
          ErrorCode::kNotFound,
          "manifest file not readable: " + path.string());
    }

    // 2. Parse JSON with nlohmann::json
    nlohmann::json j;
    try {
      f >> j;
    } catch (const nlohmann::json::parse_error& e) {
      return Result<ManifestSummary>::Err(
          ErrorCode::kInvalidArgument,
          "manifest JSON parse error: " + std::string(e.what()));
    }

    // 3. Extract all structured fields
    auto parse_result = parse_manifest_json(j);
    if (parse_result.is_err()) {
      return parse_result;
    }
    ManifestSummary summary = parse_result.take_value();

    // 4. Store in the index (atomic publication per identity/revision)
    auto key = make_key(summary.model_id, summary.revision);
    if (index_.count(key)) {
      return Result<ManifestSummary>::Err(
          ErrorCode::kAlreadyExists,
          "manifest revision already published: " + key);
    }
    index_[key] = summary;
    return Result<ManifestSummary>::Ok(summary);
  }

  Result<ManifestSummary> resolve(const ModelId& model_id,
                                  const std::string& revision) const override {
    std::lock_guard lock(mutex_);
    auto it = index_.find(make_key(model_id, revision));
    if (it == index_.end()) {
      return Result<ManifestSummary>::Err(
          ErrorCode::kNotFound,
          "manifest not found: " + model_id.value + "/" + revision);
    }
    return Result<ManifestSummary>::Ok(it->second);
  }

  std::size_t entry_count() const {
    std::lock_guard lock(mutex_);
    return index_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, ManifestSummary> index_;
};

std::unique_ptr<ManifestRepository> make_manifest_repository() {
  return std::make_unique<ManifestRepositoryImpl>();
}

}  // namespace socrates::model
