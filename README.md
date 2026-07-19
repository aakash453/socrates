# Socrates — Distributed Edge AI Runtime

**Run large language models across your devices. Mac + Windows laptops become a single inference cluster.**

Socrates splits transformer models across the devices on your desk. A Mac (master) and two Windows ThinkPads become a 36GB inference cluster. No cloud. No subscription. Just your hardware.

## Quick Start

**Master (Mac):**
```bash
./socrates-master
```
Downloads models from HuggingFace, starts HTTP API on port 8080.

**Worker (Windows — Snapdragon X Elite or x64):**
```powershell
.\setup_windows.ps1          # one-time setup
.\build_windows.ps1          # build
.\socrates-worker.exe --no-discovery
```

**Send prompts from anywhere:**
```bash
curl -X POST http://mac-ip:8080/generate \
  -H "Content-Type: application/json" \
  -d '{"model_id":"qwen3-1.8b","prompt":"Hello","max_tokens":50}'
```

## Architecture

```
┌─────────────────────────────────────────────────┐
│  Mac (Master) — 4GB, MLX GPU                    │
│  ./socrates-master                               │
│  HTTP API :8080  |  Downloads models             │
│  Gets fewer layers (lighter device)              │
└─────────────────┬───────────────────────────────┘
                  │ UDP discovery
        ┌─────────┴─────────┐
        ▼                   ▼
┌──────────────────┐ ┌──────────────────┐
│  Windows Worker 1 │ │  Windows Worker 2 │
│  16GB, llama.cpp  │ │  16GB, llama.cpp  │
│  (or NPU with SDK) │ │  (or NPU with SDK) │
│  Gets more layers  │ │  Gets more layers  │
└──────────────────┘ └──────────────────┘
```

## Supported Models

| Model | Size (INT4) | Min Devices | Download |
|---|---|---|---|
| Qwen 2.5 1.5B | 1.0 GB | 1 | ✅ HuggingFace |
| Qwen 2.5 3B | 2.0 GB | 2 | ✅ HuggingFace |
| Qwen 2.5 7B | 4.5 GB | 3 | ✅ HuggingFace |
| Llama 3 8B | 5.0 GB | 3 | ✅ HuggingFace |
| Gemma 2 9B | 5.5 GB | 3 | ✅ HuggingFace |
| Gemma 2 27B | 16 GB | 3+ | ✅ HuggingFace |

[CONTRIBUTING.md](CONTRIBUTING.md)
# socrates
