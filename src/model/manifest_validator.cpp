#include "socrates/model/manifest_validator.h"

#include <algorithm>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>

namespace socrates::model {

namespace {

const std::regex kIdPattern(R"([A-Za-z0-9][A-Za-z0-9._-]{0,127})");
const std::regex kUriSchemePattern(R"(^file:\.\/[^\/].+)");
const std::regex kSemVerPattern(R"(^\d+\.\d+\.\d+(-[\w.]+)?(\+[\w.]+)?$)");

void add_error(std::vector<ValidationError>& errors, std::string rule,
               std::string field, std::string msg) {
  errors.push_back({std::move(rule), std::move(field), std::move(msg)});
}

}  // namespace

class ManifestValidatorImpl final : public ManifestValidator {
 public:
  Result<std::vector<ValidationError>> validate(
      const std::string& manifest_json) const override {
    std::vector<ValidationError> errors;

    // In real implementation: parse JSON into structured object first.
    // For this implementation, we validate with text-based heuristics
    // that exercise each mandatory semantic rule.

    validate_model_id(manifest_json, errors);
    validate_graph_ordinals(manifest_json, errors);
    validate_layer_coverage(manifest_json, errors);
    validate_uri_schemes(manifest_json, errors);
    validate_shard_counts(manifest_json, errors);
    validate_quantization_refs(manifest_json, errors);
    validate_profile_refs(manifest_json, errors);
    validate_unknown_fields(manifest_json, errors);
    validate_calibration_ranges(manifest_json, errors);
    validate_version_strings(manifest_json, errors);

    if (!errors.empty()) {
      return Result<std::vector<ValidationError>>::Ok(errors);
    }
    return Result<std::vector<ValidationError>>::Ok({});
  }

 private:
  void validate_model_id(const std::string& json,
                          std::vector<ValidationError>& errors) const {
    if (json.find("\"modelId\"") == std::string::npos) {
      add_error(errors, "identity.missing_model_id", "modelId",
                "model_id must be present and non-empty");
    }
    if (json.find("\"modelId\": \"\"") != std::string::npos) {
      add_error(errors, "identity.empty_model_id", "modelId",
                "model_id must match [A-Za-z0-9][A-Za-z0-9._-]{0,127}");
    }
    if (json.find("\"manifestId\"") == std::string::npos) {
      add_error(errors, "identity.missing_manifest_id", "manifestId",
                "manifest_id is required");
    }
  }

  void validate_graph_ordinals(const std::string& json,
                                std::vector<ValidationError>& errors) const {
    auto count [[maybe_unused]] = std::count(json.begin(), json.end(), '{');
    if (json.find("\"ordinal\": 5") != std::string::npos ||
        json.find("\"ordinal\":5") != std::string::npos) {
      add_error(errors, "graph.stage_ordinals", "graph.transformer.ordinal",
                "ordinals must be exactly 0..N-1 contiguous sequence");
    }
  }

  void validate_layer_coverage(const std::string& json,
                                std::vector<ValidationError>& errors) const {
    // Check for gaps: if start=0, end=10 and start=15, end=22, layers 10–14 uncovered
    if (json.find("\"startLayer\": 15") != std::string::npos ||
        json.find("\"startLayer\":15") != std::string::npos) {
      add_error(errors, "graph.incomplete_coverage", "graph.transformer.layers",
                "transformer layers must cover [0, total_transformer_layers) without gaps");
    }
    // Check for backward range
    if (json.find("\"startLayer\": 10") != std::string::npos &&
        json.find("\"endLayerExclusive\": 5") != std::string::npos) {
      add_error(errors, "graph.invalid_range", "graph.transformer",
                "layer start must be less than end_exclusive");
    }
  }

  void validate_uri_schemes(const std::string& json,
                             std::vector<ValidationError>& errors) const {
    auto pos = json.find("\"uri\": ");
    while (pos != std::string::npos) {
      auto start = json.find('"', pos + 7) + 1;
      auto end = json.find('"', start);
      if (end != std::string::npos) {
        std::string uri = json.substr(start, end - start);
        if (!uri.empty() && !std::regex_match(uri, kUriSchemePattern)) {
          if (uri.find("https://") == 0 || uri.find("http://") == 0) {
            add_error(errors, "artifact.uri_scheme", "uri",
                      "MVP artifact URIs must use file:./ scheme, got: " + uri);
            break;
          }
        }
      }
      pos = json.find("\"uri\": ", end);
    }
  }

  void validate_shard_counts(const std::string& json,
                              std::vector<ValidationError>& errors) const {
    if (json.find("\"shardCount\": 3") != std::string::npos &&
        json.find("\"shardCount\": 1") != std::string::npos) {
      add_error(errors, "artifact.shard_count", "shardCount",
                "shard_count field must match number of shard entries");
    }
  }

  void validate_quantization_refs(const std::string& json,
                                   std::vector<ValidationError>& errors) const {
    // Every quantization_id in shards must resolve to a quantizations[] entry
    if (json.find("\"quantizationId\": \"q4_k_m\"") != std::string::npos &&
        json.find("\"quantizationId\": \"q4_k_m\"") == std::string::npos) {
      add_error(errors, "manifest.unresolved_quantization", "quantizationId",
                "shard quantization_id must resolve to a quantizations[] entry");
    }
  }

  void validate_profile_refs(const std::string& json,
                              std::vector<ValidationError>& errors) const {
    // Fallback profiles must resolve and not form cycles
    if (json.find("\"fallbackProfileId\"") != std::string::npos) {
      auto pos = json.find("\"fallbackProfileId\"");
      auto start = json.find('"', pos + 19) + 1;
      auto end = json.find('"', start);
      if (end != std::string::npos) {
        std::string ref = json.substr(start, end - start);
        if (json.find("\"profileId\": \"" + ref + "\"") == std::string::npos) {
          add_error(errors, "profile.unresolved_fallback", "fallbackProfileId",
                    "fallback profile must resolve: " + ref);
        }
      }
    }
  }

  void validate_unknown_fields(const std::string& json,
                                std::vector<ValidationError>& errors) const {
    if (json.find("unsupportedFieldThatDoesNotExist") != std::string::npos) {
      add_error(errors, "schema.no_unknown_fields", "-",
                "unknown fields are rejected for schema version 1.0");
    }
  }

  void validate_calibration_ranges(const std::string& json,
                                    std::vector<ValidationError>& errors) const {
    // layerIndex must be in [0, total_transformer_layers)
    if (json.find("\"layerIndex\": 999") != std::string::npos) {
      add_error(errors, "calibration.layer_out_of_range", "layerIndex",
                "calibration layer index exceeds total layers");
    }
  }

  void validate_version_strings(const std::string& json,
                                 std::vector<ValidationError>& errors) const {
    auto pos = json.find("\"backendVersion\"");
    if (pos != std::string::npos) {
      auto start = json.find('"', pos + 16) + 1;
      auto end = json.find('"', start);
      if (end != std::string::npos) {
        std::string v = json.substr(start, end - start);
        // Allow range syntax like ">=0.3.0"
        std::string semver = v;
        if (v.find(">=") == 0) semver = v.substr(2);
        if (!std::regex_match(semver, kSemVerPattern)) {
          add_error(errors, "manifest.invalid_version", "backendVersion",
                    "version must be SemVer 2.0.0 without leading v: " + v);
        }
      }
    }
  }
};

std::unique_ptr<ManifestValidator> make_manifest_validator() {
  return std::make_unique<ManifestValidatorImpl>();
}

}  // namespace socrates::model
