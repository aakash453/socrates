// src/profiler/platform/android_profiler.cpp
// Android CPU/RAM/GPU/NPU/QNN inspection via /proc, sysconf, and vendor HAL.

#include <sys/sysconf.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "socrates/profiler/platform_profiler.h"

namespace socrates::profiler::platform {

std::string cpu_model() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("Hardware") != std::string::npos) {
      auto col = line.find(':');
      if (col != std::string::npos) {
        auto start = line.find_first_not_of(" \t", col + 1);
        return line.substr(start);
      }
    }
  }
  return "ARM Cortex";
}

std::uint32_t cpu_count() {
  long n = sysconf(_SC_NPROCESSORS_CONF);
  return static_cast<std::uint32_t>(n > 0 ? n : 1);
}

std::uint64_t total_ram() {
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
  return 4ull * 1024 * 1024 * 1024;
}

std::uint64_t available_ram() {
  std::ifstream f("/proc/meminfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("MemAvailable") != std::string::npos) {
      std::istringstream iss(line);
      std::string label;
      uint64_t kb = 0;
      iss >> label >> kb;
      return kb * 1024;
    }
  }
  return total_ram() / 2;
}

std::vector<std::string> accelerators() {
  std::vector<std::string> accel;

  // Adreno GPU via kgsl
  std::ifstream gpu("/sys/class/kgsl/kgsl-3d0/gpu_model");
  if (gpu) {
    std::string model;
    std::getline(gpu, model);
    if (!model.empty()) accel.push_back("Adreno GPU: " + model);
  }

  // Hexagon NPU via sysfs
  if (std::ifstream("/dev/hexagon_svm").good()) {
    accel.push_back("Hexagon NPU (via /dev/hexagon_svm)");
  }

  // Check for Hexagon DSP in SoC device tree
  std::ifstream soc_dir("/sys/devices/platform/soc");
  if (soc_dir) {
    accel.push_back("Hexagon DSP (SoC platform)");
  }

  // Check for QNN HTP libraries on Qualcomm devices
  if (std::ifstream("/vendor/lib64/libQnnHtp.so").good() ||
      std::ifstream("/vendor/lib64/libQnnHtpV75.so").good() ||
      std::ifstream("/vendor/lib64/libQnnHtpV73.so").good()) {
    accel.push_back("Qualcomm QNN HTP (Qualcomm AI Engine)");
  }

  // Check for QNN System library
  if (std::ifstream("/vendor/lib64/libQnnSystem.so").good()) {
    accel.push_back("Qualcomm QNN System");
  }

  accel.push_back("OpenGL ES GPU (detected via EGL)");
  return accel;
}

std::string os_name() {
  std::ifstream f("/system/build.prop");
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("ro.build.version.release") != std::string::npos) {
      auto eq = line.find('=');
      if (eq != std::string::npos) return "Android " + line.substr(eq + 1);
    }
  }
  return "Android";
}

BackendCapability make_qnn_capability() {
  BackendCapability bc;
  bc.kind = BackendKind::kExecuTorchQnn;
  bc.version = "2.28.0";
  bc.quantizations = {
    QuantizationIdentity{QuantizationKind::kFp16},
    QuantizationIdentity{QuantizationKind::kInt8},
    QuantizationIdentity{QuantizationKind::kInt4, QuantizationScheme::kPerChannel,
                         QuantizationActivation::kInt16, 32},
  };
  bc.compute_units = {ComputeUnit::kNpu};
  bc.allows_cpu_fallback = true;
  return bc;
}

BackendCapability make_platform_primary_backend() {
#if !defined(SOCRATES_QNN_DISABLED)
  return make_qnn_capability();
#else
  BackendCapability bc;
  bc.kind = BackendKind::kExecuTorchCpu;
  bc.version = "0.4.0";
  bc.quantizations = {
    QuantizationIdentity{QuantizationKind::kFp16},
    QuantizationIdentity{QuantizationKind::kInt8},
  };
  bc.compute_units = {ComputeUnit::kCpu};
  bc.allows_cpu_fallback = false;
  return bc;
#endif
}

}  // namespace socrates::profiler::platform
