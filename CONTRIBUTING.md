# Contributing to Socrates

## macOS

```bash
brew install cmake ninja conan
conan install . --output-folder=build/conan-release --build=missing -s build_type=Release
cmake --preset macos-release
cmake --build --preset macos-release --parallel $(sysctl -n hw.logicalcpu)

# Output: build/macos-release/tools/socrates-master
#         build/macos-release/tools/socrates-worker
```

## Windows (Snapdragon X Elite or x64)

**One-time setup:**
```powershell
.\setup_windows.ps1
```

**Build:**
```powershell
.\build_windows.ps1
# Output: build/windows-arm64-release/tools/socrates-worker.exe
```

**With Qualcomm QNN NPU:** install the [QNN SDK](https://developer.qualcomm.com/software/qualcomm-neural-processing-sdk), set `$env:QNN_SDK_ROOT`, then build with:
```powershell
.\build_windows.ps1 -WithQNN
```

Without the SDK, falls back to llama.cpp CPU — works on any Windows ARM64 or x64 machine.

## Project Structure

```
socrates/
├── include/socrates/     # Public headers
│   ├── cluster/          # Membership, election
│   ├── discovery/        # mDNS, UDP, BLE
│   ├── inference/        # Backend interface, registry
│   ├── pipeline/         # Inference pipeline, batching
│   ├── profiler/         # Hardware profiling
│   ├── runtime/          # EdgeRuntime, model catalog
│   ├── scheduler/        # Memory scheduler
│   ├── security/         # Identity, X.509
│   └── transport/        # TensorCodec, TLS transport
├── src/                  # Implementation
├── tools/                # master_cli, worker_cli
├── dist/                 # Prebuilt binaries
├── model-repo/           # Model manifests
└── CMakePresets.json     # Build presets
```

## Writing Tests

```bash
conan install . --output-folder=build/conan-debug --build=missing -s build_type=Debug
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --test-dir build/macos-debug --output-on-failure
```
