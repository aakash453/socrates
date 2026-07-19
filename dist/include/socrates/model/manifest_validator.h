// include/socrates/model/manifest_validator.h
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "socrates/result.h"

namespace socrates::model {

struct ValidationError {
  std::string rule;
  std::string field;
  std::string message;
};

class ManifestValidator {
 public:
  virtual ~ManifestValidator() = default;

  /**
   * Validates all mandatory semantic rules against a parsed manifest.
   * Preconditions: manifest has been parsed from a verified envelope.
   * Postconditions: returns ok for a fully valid manifest, or a list of
   * specific rules violated.
   * Throws: no operational exceptions.
   * Thread safety: reentrant, no mutable state.
   * Side effects: none.
   */
  virtual Result<std::vector<ValidationError>> validate(const std::string& manifest_json) const = 0;
};

std::unique_ptr<ManifestValidator> make_manifest_validator();

}  // namespace socrates::model
