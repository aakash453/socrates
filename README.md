# Socrates — Distributed Edge AI Runtime

**Your laptop + your friend's laptop + your old desktop = one GPU cluster.**

Socrates lets you run big LLMs across the devices you already own. It splits transformer layers across machines and passes hidden states between them — so a 27B model runs on 3 laptops, not a datacenter.

---

## The Idea

Cloud GPUs are expensive. Your desk has 3-4 computers with 4-16GB RAM each. None of them can run a 27B model alone. But together they've got 36+ GB — enough to run something serious.

Socrates does pipeline-parallel inference: each machine owns a slice of the model's layers. The first machine embeds your prompt, runs layers 0-8, ships the hidden state to machine 2, which runs layers 9-19, ships to machine 3, which finishes and outputs a token. Repeat.

No cloud. No API keys. No subscription. Your hardware, your data.

## How It Works (the casual version)

1. You start the **master** on your Mac. It downloads a bunch of GGUF models, fires up an HTTP API, and starts listening for other machines via UDP broadcast.

2. You start **workers** on your Windows laptops (or other Macs). They download the same models, discover the master, and wait.

3. A worker presses `j` to join the cluster. The master profiles everyone's available RAM, checks the model manifest, and assigns each machine a slice of layers ("you take 0-8, you take 9-19, you take 20-32").

4. You send a prompt via curl (or anything that speaks HTTP). The master tokenizes it, runs the first layers, ships hidden states to worker 1 via TLS, worker 1 runs its layers, ships to worker 2, worker 2 produces a token — and the token streams back to you via SSE.

5. If a machine drops, the scheduler rebalances. If you want batch-size-1 (longer context), flip a flag. If you want to benchmark GPU/NPU miss rates, use the profiler model.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Any client (curl, app, browser)       │
│                    HTTP POST /generate                  │
└────────────────────────┬────────────────────────────────┘
                         │ SSE (token stream)
                         ▼
┌─────────────────────────────────────────────────────────┐
│  Mac (Master + HTTP API)                                │
│  ./socrates-master --no-discovery                        │
│  ┌───────────────────────────────────────────────────┐  │
│  │ Runtime: identity → discovery → membership →      │  │
│  │ election → profiler → scheduler → backends →      │  │
│  │ transport → pipeline                              │  │
│  └───────────────────────────────────────────────────┘  │
│  Layers 0-8 (MLX Metal GPU)                             │
│  4 GB RAM budget                                        │
└──────┬──────────────────────────────────────┬───────────┘
       │ hidden state (TLS 1.3, :9876)        │
       ▼                                      ▼
┌──────────────────────┐          ┌──────────────────────┐
│  Windows ThinkPad 1  │          │  Windows ThinkPad 2  │
│  llama.cpp CPU       │          │  llama.cpp CPU       │
│  Layers 9-19         │          │  Layers 20-32        │
│  16 GB RAM           │          │  16 GB RAM           │
│  (QNN NPU optional)  │          │  (QNN NPU optional)  │
└──────────────────────┘          └──────────────────────┘
```

## Subsystems

| Layer | What it does |
|---|---|
| **Discovery** | UDP broadcast + BLE fallback — finds nearby machines without config |
| **Membership** | Tracks who's alive, joining, suspect, or gone. Heartbeats + authentication |
| **Election** | Fenced bully algorithm — picks a leader, prevents split-brain |
| **Profiler** | Queries RAM, CPU cores, GPU/NPU availability per machine |
| **Scheduler** | Reads model manifest, divides layers across devices based on available RAM |
| **Pipeline** | Worker threads, batching modes, cancellation, KV replay on failover |
| **Transport** | TLS 1.3 binary protocol — ships hidden state tensors between pipeline stages |
| **Inference** | llama.cpp (CPU/GPU), MLX (Apple Silicon), LiteRT/QNN (Snapdragon NPU) |
| **Persistence** | SQLite — stores assignment state, recovers on restart |
| **Security** | X.509 P-256 self-signed certificates, ephemeral trust-on-first-use |

## Terminology

| Term | Meaning |
|---|---|
| **Master** | The machine running `socrates-master`. Runs the HTTP API, acts as cluster leader, schedules work |
| **Worker** | Any machine running `socrates-worker`. Downloads models, joins the cluster, runs assigned layers |
| **Leech** | A node that can send prompts but doesn't run inference layers (default for new workers) |
| **Participant** | A node that has joined the cluster and runs assigned layers (press `j` to become one) |
| **Pipeline plan** | The scheduler's output: which machine runs which layers, what backend, how much RAM |
| **Hidden state** | The intermediate tensor passed between pipeline stages. The actual data flowing between machines |
| **Shard** | A slice of a model's layers assigned to one machine |
| **Manifest** | A JSON file (`model-repo/manifest.json`) describing each model's layers, sizes, and compatible cluster configs |
| **GGUF** | The file format for quantized models that llama.cpp loads |

## Supported Models

All from HuggingFace, auto-downloaded on first run:

| Model | `model_id` | Size (INT4) | Min Cluster | Use Case |
|---|---|---|---|---|
| Qwen 2.5 0.5B | `qwen3-1.8b` | 374 MB | 1 device | Profiling, benchmarking |
| Qwen 2.5 1.5B | `qwen3-1.8b` | 986 MB | 1 device | Quick testing |
| Qwen 2.5 3B | `qwen3-4b` | 1.4 GB | 2 devices | Light chat |
| Qwen 2.5 7B | `qwen3-6b` | 4.7 GB | 3 devices | General purpose |
| Llama 3 8B | `llama3-8b` | 4.9 GB | 3 devices | Strong reasoning |
| Gemma 2 9B | `gemma12b` | 5.7 GB | 3 devices | Code, analysis |
| Gemma 2 27B | `gemma26b` | 10.7 GB | 3+ devices | Heavy lifting |
| Gemma 4 26B | `gemma4-26b` | 15 GB | 3+ devices | Latest Gen |

## Quick Start

### Run the Master (Mac)

```bash
cd socrates
./dist/macos-arm64/socrates-master --no-discovery
```

Downloads models to `/tmp/socrates-master/models/`. Starts HTTP API on `:8080`.

### Run a Worker (Mac, same machine — for testing)

```bash
./dist/macos-arm64/socrates-worker --no-discovery --skip-downloads
```

Press `j` to join the cluster. Press `s` for status. Press `q` to quit.

### Run a Worker (Windows ThinkPad)

```powershell
.\setup_windows.ps1            # one-time: installs CMake, Ninja, VS
.\build_windows.ps1            # compile
.\build\windows-arm64-release\tools\socrates-worker.exe --no-discovery
```

For NPU acceleration (Snapdragon X Elite):
```powershell
.\build_windows.ps1 -WithQNN    # requires Qualcomm QNN SDK
```

### Send Prompts

```bash
# Find your Mac's IP
ipconfig getifaddr en0

# Health check
curl http://<mac-ip>:8080/health

# See all devices in the cluster
curl http://<mac-ip>:8080/cluster

# Available models for this cluster config
curl http://<mac-ip>:8080/models

# Generate!
curl -X POST http://<mac-ip>:8080/generate \
  -H "Content-Type: application/json" \
  -d '{"model_id":"llama3-8b","prompt":"Explain async Rust in one paragraph.","max_tokens":100}'
```

## API Reference

| Method | Endpoint | Body | Response |
|---|---|---|---|
| `GET` | `/health` | — | `{"status":"running","version":"0.2.0"}` |
| `GET` | `/cluster` | — | JSON: devices, roles, pipeline stages, leader |
| `GET` | `/models` | — | JSON: models compatible with current cluster |
| `POST` | `/generate` | `{"model_id","prompt","max_tokens"}` | SSE stream: `data: {"token":"...","seq":N}` → `data: {"done":true}` |
| `POST` | `/join` | — | `{"ok":true}` — promotes leech to participant |
| `POST` | `/cancel` | `{"generation_id"}` | `{"ok":true}` |

## How to Set Up, Build, and Test

See **[CONTRIBUTING.md](CONTRIBUTING.md)** for:

- macOS build from source (CMake + Conan + Ninja)
- Windows build (PowerShell scripts, QNN SDK setup)
- Running tests (82 unit tests pass on macOS)
- Project structure
- Debugging tips (mDNS hangs, discovery port conflicts)

TL;DR for macOS:
```bash
brew install cmake ninja conan protobuf grpc
conan install . --output-folder=build/conan-release --build=missing -s build_type=Release
cmake --preset macos-release
cmake --build --preset macos-release --parallel $(sysctl -n hw.logicalcpu)
ctest --test-dir build/macos-debug --output-on-failure
```

## What's Real vs. Simulated

- ✅ UDP discovery (real sockets, broadcast + receive)
- ✅ TLS 1.3 transport (real OpenSSL, port :9876)
- ✅ llama.cpp inference (real GGUF loading, Metal GPU on Mac)
- ✅ Membership heartbeats (real state machine)
- ✅ Pipeline scheduling (real plan creation, layer assignment)
- ✅ Model downloads (real curl to HuggingFace)
- ✅ SSE streaming (real HTTP responses)
- ⚠️ LiteRT/QNN (stub — install SDK for real NPU)
- ⚠️ BLE discovery (stub — platform-specific)
