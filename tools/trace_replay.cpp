// tools/trace_replay.cpp
// Trace replay and summary CLI.

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char* argv[]) {
  bool summary_only = false;
  std::string path;

  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--summary") summary_only = true;
    else if (arg[0] != '-') path = arg;
  }

  if (path.empty()) {
    std::cerr << "Usage: socrates-trace-replay [--summary] <trace.pb>" << std::endl;
    return 1;
  }

  std::ifstream f(path);
  if (!f) {
    std::cerr << "Cannot open " << path << std::endl;
    return 1;
  }

  std::ostringstream buf;
  buf << f.rdbuf();
  auto content = buf.str();

  std::cerr << "Trace file loaded: " << content.size() << " bytes" << std::endl;

  if (summary_only) {
    std::cerr << "Summary: "
              << "format=protobuf-binary, "
              << "size=" << content.size() << " bytes" << std::endl;
    return 0;
  }

  std::cerr << "Trace replay not yet implemented." << std::endl;
  return 0;
}
