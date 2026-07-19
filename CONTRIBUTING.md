# Contributing to Socrates

## Prerequisites (all platforms)

- CMake >= 3.22
- Ninja
- Conan 2.x
- A C++20 compiler

## macOS Build & Test

### One-time setup

```bash
brew install cmake ninja conan protobuf grpc
```

### Build (Release)

```bash
conan install . --output-folder=build/conan-release --build=missing -s build_type=Release
cmake --preset macos-release
cmake --build --preset macos-release --parallel $(sysctl -n hw.logicalcpu)
```

Outputs:
- `build/macos-release/tools/socrates-master` — HTTP API server
- `build/macos-release/tools/socrates-worker` — CLI that joins the cluster
- `build/macos-release/libsocrates.a` — static library

### Build (Debug + Tests)

```bash
conan install . --output-folder=build/conan-debug --build=missing -s build_type=Debug
cmake --preset macos-debug
cmake --build --preset macos-debug --parallel $(sysctl -n hw.logicalcpu)
```

### Run Tests

```bash
ctest --test-dir build/macos-debug --output-on-failure -j $(sysctl -n hw.logicalcpu)
```

82 tests across unit, contract, integration, and e2e suites.

### Prebuilt Binaries

If you don't want to build: `dist/macos-arm64/` has the latest `socrates-master` and `socrates-worker`. Just run them directly.

---

## Windows Build (Snapdragon X Elite / ARM64)

### One-time setup

```powershell
.\setup_windows.ps1
```

Installs CMake, Ninja, and Visual Studio Build Tools (ARM64). Requires `winget`.

### Build (CPU only)

```powershell
.\build_windows.ps1
# Output: build\windows-arm64-release\tools\socrates-worker.exe
```

### Build with Qualcomm QNN NPU

1. Download and install [Qualcomm QNN SDK](https://developer.qualcomm.com/software/qualcomm-neural-processing-sdk) (free account required)
2. Set the environment variable:
   ```powershell
   $env:QNN_SDK_ROOT = "C:\Qualcomm\QNN\2.28.0"
   ```
3. Build:
   ```powershell
   .\build_windows.ps1 -WithQNN
   ```

The QNN backend tries NPU first, falls back to llama.cpp CPU if the NPU is unavailable or the SDK isn't installed.

### Build for x64

```powershell
.\build_windows.ps1 -Arch x64
```

### Runtime Dependencies

The `.exe` is statically linked and self-contained. The only runtime dependency is:
- **curl** (ships with Windows 10/11) — for downloading models from HuggingFace

Models are downloaded to `C:\tmp\socrates-worker\models\` on first run (~34 GB for all models). Use `--skip-downloads` to skip.

---

## Project Structure

```
socrates/
├── include/socrates/       # Public headers
│   ├── cluster/            # Membership, election
│   ├── discovery/          # mDNS, UDP, BLE
│   ├── inference/          # Backend interface, registry
│   ├── pipeline/           # Inference pipeline, batching
│   ├── profiler/           # Hardware profiling
│   ├── runtime/            # EdgeRuntime, model catalog
│   ├── scheduler/          # Memory scheduler
│   ├── security/           # Identity, X.509
│   └── transport/          # TensorCodec, TLS transport
├── src/                    # Implementation
│   ├── cluster/            # bully_election, membership_service
│   ├── discovery/          # udp_discovery, mdns, bluetooth
│   ├── inference/          # llama_backend, mlx_backend, executorch
│   ├── pipeline/           # inference_pipeline
│   ├── profiler/           # capability_profiler
│   ├── runtime/            # edge_runtime, backend_dispatch, model_catalog
│   ├── scheduler/          # memory_scheduler
│   ├── security/           # identity_provider
│   └── transport/          # grpc_transport, tensor_codec
├── tools/                  # CLIs
│   ├── master_cli.cpp      # HTTP API server
│   └── worker_cli.cpp      # Discovery + join worker
├── bindings/c/             # C ABI (consumed by CLIs)
├── dist/                   # Prebuilt binaries
│   └── macos-arm64/        # socrates-master, socrates-worker
├── model-repo/             # Model manifests (JSON)
├── tests/                  # unit, contract, integration, e2e
├── cmake/                  # FindQNN, CompilerWarnings, etc.
├── CMakePresets.json       # Build presets (macos, windows, linux)
└── conanfile.py            # Dependencies (spdlog, nlohmann_json, etc.)
```

---

## Debugging Tips

### mDNS hangs on macOS

`DNSServiceRegister` (Bonjour) blocks in sandboxed environments. Use `--no-discovery` on both master and worker to skip mDNS and use UDP broadcast instead.

### "Address already in use" on port 9876

Both master and worker bind UDP port 9876 for discovery. On the same machine, only one can bind. Use `SO_REUSEPORT` (already enabled). On separate machines this isn't an issue.

### "decode: n_tokens == 0"

llama.cpp's `llama_batch_init()` allocates token slots but doesn't set the `n_tokens` field — it must be set explicitly after populating the batch. Fixed in `inference_pipeline.cpp`.

### Thread safety in callbacks

Callbacks (membership, election, pipeline) must never re-acquire mutexes already held by the caller. Three separate deadlocks were fixed during development — all caused by `election_->current()` being called from within election callbacks.

---

## Key Design Decisions

1. **New nodes start as leeches** — they can send prompts but not run inference. Must explicitly `join_cluster()` to become a participant.
2. **Pipeline-parallel, not tensor-parallel** — layers are split across machines, not within a single layer. Hidden states flow between machines, not weights.
3. **Model shards are layer ranges, not weight slices** — each machine runs a contiguous block of transformer layers.
4. **Discovery is not trust** — UDP broadcast finds candidates; X.509 certificates authenticate them.
5. **Elections are fenced** — all leader-authored mutations carry `(term, fencing_token, membership_revision)` and stale mutations are rejected.
