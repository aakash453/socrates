// src/profiler/platform/linux_profiler.cpp
// Linux/container CPU/RAM/GPU inspection via /proc, /sys, cgroups, and lspci.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "socrates/profiler/platform_profiler.h"

namespace socrates::profiler::platform {

namespace {

uint64_t read_uint64_file(const char* path) {
  std::ifstream f(path);
  uint64_t val = 0;
  f >> val;
  return val;
}

std::string read_string_file(const char* path) {
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  return s;
}

}  // namespace

std::string cpu_model() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("model name") != std::string::npos) {
      auto col = line.find(':');
      if (col != std::string::npos) {
        auto start = line.find_first_not_of(" \t", col + 1);
        return line.substr(start);
      }
    }
  }
  return "x86_64";
}

std::uint32_t cpu_count() {
  return static_cast<std::uint32_t>(
      std::thread::hardware_concurrency());
}

std::uint64_t total_ram() {
  // Check cgroup v2 limit first (container-aware)
  std::ifstream cg("/sys/fs/cgroup/memory.max");
  if (cg) {
    std::string val;
    std::getline(cg, val);
    if (val != "max") {
      return std::stoull(val);
    }
  }
  // Fall back to /proc/meminfo — MemTotal is in kB
  std::ifstream f("/proc/meminfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("MemTotal") != std::string::npos) {
      std::istringstream iss(line);
      std::string label;
      uint64_t kb = 0;
      iss >> label >> kb;
      return kb * 1024;
    }
  }
  return 8ull * 1024 * 1024 * 1024;
}

std::uint64_t available_ram() {
  // cgroup-aware available
  std::ifstream cg("/sys/fs/cgroup/memory.current");
  if (cg) {
    uint64_t current = 0;
    cg >> current;
    return total_ram() > current ? total_ram() - current : 0;
  }
  return total_ram() / 2;
}

std::vector<std::string> accelerators() {
  std::vector<std::string> accel;
  // NVIDIA GPU via /proc/driver/nvidia/gpus
  std::ifstream nv("/proc/driver/nvidia/gpus/0/information");
  if (nv) {
    std::string info = read_string_file("/proc/driver/nvidia/gpus/0/information");
    accel.push_back("NVIDIA GPU: " + info);
  }
  // Intel GPU via sysfs
  std::ifstream vendor_file("/sys/class/drm/card0/device/vendor");
  if (vendor_file) {
    auto vendor = read_string_file("/sys/class/drm/card0/device/vendor");
    if (vendor.find("0x8086") != std::string::npos) {
      accel.push_back("Intel GPU (i915)");
    }
  }
  return accel;
}

std::string os_name() {
  std::ifstream f("/etc/os-release");
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("PRETTY_NAME") != std::string::npos) {
      auto eq = line.find('=');
      if (eq != std::string::npos) {
        auto start = line.find('"', eq);
        auto end = line.find('"', start + 1);
        if (start != std::string::npos && end != std::string::npos) {
          return line.substr(start + 1, end - start - 1);
        }
      }
    }
  }
  return "Linux";
}

BackendCapability make_platform_primary_backend() {
  BackendCapability bc;
  bc.kind = BackendKind::kLlamaCpp;
  bc.version = "0.3.0+cuda12";
  bc.quantizations = {
    QuantizationIdentity{QuantizationKind::kFp16},
    QuantizationIdentity{QuantizationKind::kInt8},
    QuantizationIdentity{QuantizationKind::kInt4, QuantizationScheme::kPerGroup,
                         QuantizationActivation::kFp16, 128},
  };
  bc.compute_units = {ComputeUnit::kCpu, ComputeUnit::kGpu};
  bc.allows_cpu_fallback = true;
  return bc;
}

}  // namespace socrates::profiler::platform
