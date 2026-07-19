// src/profiler/platform/windows_profiler.cpp
// Windows CPU/RAM/GPU/NPU inspection via Win32 API.
// ARM64 path: Qualcomm Hexagon NPU and QNN detection.

#include <windows.h>
#include <sysinfoapi.h>
#include <psapi.h>
#include <dxgi.h>

#include <string>
#include <vector>

#include "socrates/profiler/platform_profiler.h"

namespace socrates::profiler::platform {

std::string cpu_model() {
  HKEY hKey;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                    0, KEY_READ, &hKey) == ERROR_SUCCESS) {
    char buf[256] = {};
    DWORD len = sizeof(buf);
    if (RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(buf), &len) == ERROR_SUCCESS) {
      RegCloseKey(hKey);
      return std::string(buf);
    }
    RegCloseKey(hKey);
  }
#ifdef _M_ARM64
  return "ARM64";
#else
  return "x86_64";
#endif
}

std::uint32_t cpu_count() {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors;
}

std::uint64_t total_ram() {
  MEMORYSTATUSEX mem;
  mem.dwLength = sizeof(mem);
  GlobalMemoryStatusEx(&mem);
  return mem.ullTotalPhys;
}

std::uint64_t available_ram() {
  MEMORYSTATUSEX mem;
  mem.dwLength = sizeof(mem);
  GlobalMemoryStatusEx(&mem);
  return mem.ullAvailPhys;
}

std::vector<std::string> accelerators() {
  std::vector<std::string> accel;

  // DXGI adapter enumeration for GPU
  IDXGIFactory* factory = nullptr;
  if (CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)) == S_OK) {
    UINT i = 0;
    IDXGIAdapter* adapter = nullptr;
    while (factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND) {
      DXGI_ADAPTER_DESC desc;
      if (adapter->GetDesc(&desc) == S_OK) {
        int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                       nullptr, 0, nullptr, nullptr);
        std::string name(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                            &name[0], len, nullptr, nullptr);
        accel.push_back("DXGI GPU: " + name);
      }
      adapter->Release();
      i++;
    }
    factory->Release();
  }

#ifdef _M_ARM64
  // Windows ARM64 (Snapdragon X Elite) — Qualcomm NPU detection

  // Check for Qualcomm device in system devices registry
  HKEY qcomKey;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "SYSTEM\\CurrentControlSet\\Enum\\QCOM",
                    0, KEY_READ, &qcomKey) == ERROR_SUCCESS) {
    accel.push_back("Qualcomm NPU (QCOM device node)");
    RegCloseKey(qcomKey);
  }

  // Check for Hexagon NPU via Qualcomm AI Engine (QNN)
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Qualcomm\\AIEngine",
                    0, KEY_READ, &qcomKey) == ERROR_SUCCESS) {
    accel.push_back("Qualcomm AI Engine (Hexagon NPU + QNN)");
    RegCloseKey(qcomKey);
  }

  // Check for Windows AI NPU (WinML API via Windows 11 AI components)
  HMODULE qnnDll = LoadLibraryA("QnnHtp.dll");
  if (qnnDll) {
    accel.push_back("Qualcomm QNN HTP (QnnHtp.dll)");
    FreeLibrary(qnnDll);
  }

  HMODULE qnnSysDll = LoadLibraryA("QnnSystem.dll");
  if (qnnSysDll) {
    accel.push_back("Qualcomm QNN System (QnnSystem.dll)");
    FreeLibrary(qnnSysDll);
  }

  // Windows AI NPU via DirectML (Windows Copilot+ PC NPU)
  HMODULE dmlDll = LoadLibraryA("DirectML.dll");
  if (dmlDll) {
    accel.push_back("Windows AI NPU (DirectML)");
    FreeLibrary(dmlDll);
  }
#endif

  return accel;
}

std::string os_name() {
  return "Windows";
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

BackendCapability make_litert_capability() {
  BackendCapability bc;
  bc.kind = BackendKind::kLiteRt;
  bc.version = "1.0.0";
  bc.quantizations = {
    QuantizationIdentity{QuantizationKind::kFp16},
    QuantizationIdentity{QuantizationKind::kInt8},
    QuantizationIdentity{QuantizationKind::kInt4, QuantizationScheme::kPerGroup,
                         QuantizationActivation::kInt16, 128},
  };
  bc.compute_units = {ComputeUnit::kGpu, ComputeUnit::kNpu};
  bc.allows_cpu_fallback = true;
  return bc;
}

BackendCapability make_platform_primary_backend() {
#ifdef _M_ARM64
  // Snapdragon X Elite: QNN NPU is primary, LiteRT GPU is secondary
#if !defined(SOCRATES_QNN_DISABLED)
  return make_qnn_capability();
#elif !defined(SOCRATES_LITERT_DISABLED)
  return make_litert_capability();
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
#else
  // x64: LiteRT GPU/NPU is primary
#if !defined(SOCRATES_LITERT_DISABLED)
  return make_litert_capability();
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
#endif
}

}  // namespace socrates::profiler::platform
