// tools/manifest_validator.cpp
// Standalone manifest validator CLI.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

#include "socrates/model/manifest_validator.h"

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: socrates-manifest-validator <manifest.json>" << std::endl;
    return 1;
  }

  std::filesystem::path path(argv[1]);
  std::ifstream f(path);
  if (!f) {
    std::cerr << "Cannot open " << path << std::endl;
    return 1;
  }

  std::ostringstream buf;
  buf << f.rdbuf();
  std::string content = buf.str();

  auto validator = socrates::model::make_manifest_validator();
  auto result = validator->validate(content);

  if (result.is_err()) {
    std::cerr << "Validation error: " << result.error().what() << std::endl;
    return 1;
  }

  auto& errors = result.value();
  if (errors.empty()) {
    std::cout << "Manifest is valid." << std::endl;
    return 0;
  }

  std::cerr << "Found " << errors.size() << " validation error(s):" << std::endl;
  for (const auto& e : errors) {
    std::cerr << "  [" << e.rule << "] " << e.field << ": " << e.message
              << std::endl;
  }
  return 1;
}
