// src/profiler/platform/apple_profiler.mm
// Apple CPU/RAM/GPU/Neural Engine inspection via IOKit, Metal, and sysctl.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>

#include <sys/sysctl.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <mach/mach_host.h>

#include <string>
#include <vector>

#include "socrates/profiler/platform_profiler.h"

namespace socrates::profiler::platform {

std::string cpu_model() {
  char buf[256];
  size_t len = sizeof(buf);
  if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) {
    return std::string(buf, len - 1);
  }
  return "Apple Silicon";
}

std::uint32_t cpu_count() {
  int count = 0;
  size_t len = sizeof(count);
  sysctlbyname("hw.logicalcpu", &count, &len, nullptr, 0);
  return static_cast<std::uint32_t>(count > 0 ? count : 1);
}

std::uint64_t total_ram() {
  uint64_t mem = 0;
  size_t len = sizeof(mem);
  sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
  return mem;
}

std::uint64_t available_ram() {
  mach_port_t host = mach_host_self();
  vm_size_t page_size;
  host_page_size(host, &page_size);

  vm_statistics64_data_t vm_stat;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm_stat),
                        &count) == KERN_SUCCESS) {
    return static_cast<std::uint64_t>(vm_stat.free_count) * page_size;
  }
  return total_ram() / 4;
}

std::vector<std::string> accelerators() {
  std::vector<std::string> accel;
  @autoreleasepool {
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    for (id<MTLDevice> device in devices) {
      std::string name = [device.name UTF8String];
      std::string flags;

      if (device.isLowPower) flags += " low-power";
      if (device.isRemovable) flags += " removable";
      if (device.isHeadless) flags += " headless";

      accel.push_back("Metal GPU: " + name + flags);

      if ([device supportsFamily:MTLGPUFamilyApple9]) {
        accel.push_back("Apple Neural Engine (A17 Pro / M4, gen 9)");
      } else if ([device supportsFamily:MTLGPUFamilyApple8]) {
        accel.push_back("Apple Neural Engine (A16 / M3, gen 8)");
      } else if ([device supportsFamily:MTLGPUFamilyApple7]) {
        accel.push_back("Apple Neural Engine (A15 / M2, gen 7)");
      }
    }

    if (devices.count == 0) {
      id<MTLDevice> defaultDevice = MTLCreateSystemDefaultDevice();
      if (defaultDevice) {
        accel.push_back("Metal GPU (default): " +
                        std::string([defaultDevice.name UTF8String]));

        if ([defaultDevice supportsFamily:MTLGPUFamilyApple9]) {
          accel.push_back("Apple Neural Engine (A17 Pro / M4)");
        } else if ([defaultDevice supportsFamily:MTLGPUFamilyApple8]) {
          accel.push_back("Apple Neural Engine (A16 / M3)");
        } else if ([defaultDevice supportsFamily:MTLGPUFamilyApple7]) {
          accel.push_back("Apple Neural Engine (A15 / M2)");
        }
      }
    }
  }
  return accel;
}

std::string os_name() {
  @autoreleasepool {
    NSProcessInfo* info = [NSProcessInfo processInfo];
    NSOperatingSystemVersion v = [info operatingSystemVersion];

#if TARGET_OS_IOS
    return "iOS " + std::to_string(v.majorVersion) + "." +
           std::to_string(v.minorVersion) + "." + std::to_string(v.patchVersion);
#else
    return "macOS " + std::to_string(v.majorVersion) + "." +
           std::to_string(v.minorVersion) + "." + std::to_string(v.patchVersion);
#endif
  }
}

BackendCapability make_mlx_capability() {
  BackendCapability bc;
  bc.kind = BackendKind::kMlx;
  bc.version = "0.20.0";
  bc.quantizations = {
    QuantizationIdentity{QuantizationKind::kFp16},
    QuantizationIdentity{QuantizationKind::kInt4, QuantizationScheme::kPerGroup,
                         QuantizationActivation::kFp16, 128},
    QuantizationIdentity{QuantizationKind::kInt8, QuantizationScheme::kPerChannel,
                         QuantizationActivation::kFp16, 128},
  };
  bc.compute_units = {ComputeUnit::kGpu, ComputeUnit::kNpu};
  bc.allows_cpu_fallback = true;
  return bc;
}

BackendCapability make_platform_primary_backend() {
#if !defined(SOCRATES_MLX_DISABLED)
  return make_mlx_capability();
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
