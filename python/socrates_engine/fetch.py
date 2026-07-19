"""socrates-fetch CLI — downloads GGUF models and creates manifests.

Supports:
  - Hugging Face Hub (requires huggingface_hub)
  - Direct URL download
  - Local file copy with manifest generation

Usage:
  socrates-fetch --model TinyLlama/TinyLlama-1.1B-Chat-v1.0 --output ./models
  socrates-fetch --auto --output ./models
  socrates-fetch --url https://example.com/model.gguf --output ./models --name my-model
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import urllib.request
from pathlib import Path


# ── Known models registry ──────────────────────────────────────────────────
# Maps model_id → { hf_repo, files, total_layers, description }

KNOWN_MODELS = {
    "tinyllama-1.1b": {
        "hf_repo": "TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF",
        "files": [
            {"filename": "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf", "shard": None, "quantization": "q4_k_m"},
        ],
        "total_layers": 22,
        "description": "TinyLlama 1.1B Chat — good for testing on edge devices",
    },
    "phi-2": {
        "hf_repo": "TheBloke/phi-2-GGUF",
        "files": [
            {"filename": "phi-2.Q4_K_M.gguf", "shard": None, "quantization": "q4_k_m"},
        ],
        "total_layers": 32,
        "description": "Microsoft Phi-2 2.7B — strong for its size",
    },
    "llama-3.2-3b": {
        "hf_repo": "bartowski/Llama-3.2-3B-Instruct-GGUF",
        "files": [
            {"filename": "Llama-3.2-3B-Instruct-Q4_K_M.gguf", "shard": None, "quantization": "q4_k_m"},
        ],
        "total_layers": 28,
        "description": "Meta Llama 3.2 3B Instruct — solid general-purpose model",
    },
}


def download_hf(repo_id: str, filename: str, output_dir: Path) -> Path:
    """Download a file from Hugging Face Hub."""
    try:
        from huggingface_hub import hf_hub_download
    except ImportError:
        print("Error: huggingface_hub not installed. Install with: pip install huggingface_hub",
              file=sys.stderr)
        sys.exit(1)

    print(f"  Downloading {filename} from {repo_id}...")
    local_path = hf_hub_download(
        repo_id=repo_id,
        filename=filename,
        local_dir=str(output_dir),
        local_dir_use_symlinks=False,
    )
    return Path(local_path)


def download_url(url: str, output_dir: Path, filename: str) -> Path:
    """Download a file from a direct URL."""
    dest = output_dir / filename
    print(f"  Downloading {url} → {dest}...")

    def report(block_num, block_size, total_size):
        downloaded = block_num * block_size
        if total_size > 0:
            pct = min(100, int(downloaded * 100 / total_size))
            print(f"\r  {pct}% ({downloaded // (1024*1024)} MB / {total_size // (1024*1024)} MB)", end="")

    urllib.request.urlretrieve(url, str(dest), reporthook=report)
    print()
    return dest


def compute_sha256(path: Path) -> str:
    """Compute SHA-256 hex digest of a file."""
    sha = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(8 * 1024 * 1024):
            sha.update(chunk)
    return sha.hexdigest()


def generate_manifest(
    model_id: str,
    model_name: str,
    files: list[dict],
    total_layers: int,
    output_dir: Path,
) -> Path:
    """Generate a manifest.json from downloaded model files.

    If there's a single .gguf file, creates a manifest with one shard.
    If there are multiple files tagged with layer ranges, creates multi-shard.
    """
    # Detect if we have sharded files or a single file
    has_shards = any(f.get("shard") is not None for f in files)

    shards = []
    quantizations = []
    seen_quants = set()

    for f in files:
        quant_id = f.get("quantization", "q4_k_m")
        if quant_id not in seen_quants:
            quantizations.append({
                "id": quant_id,
                "kind": "int4" if "q4" in quant_id else "fp16",
                "scheme": "perGroup",
                "activation": "fp16",
                "groupSize": 128,
            })
            seen_quants.add(quant_id)

        filepath = output_dir / f["filename"]
        sha256 = compute_sha256(filepath) if filepath.exists() else "unknown"
        size = filepath.stat().st_size if filepath.exists() else 0

        shard_info = f.get("shard")
        if shard_info:
            shards.append({
                "shardId": f"shard-{shard_info['start']}-{shard_info['end']}",
                "stageIds": [f"transformer-layers-{shard_info['start']}-{shard_info['end']}"],
                "stageKind": "transformerLayers",
                "layers": {"start": shard_info["start"], "endExclusive": shard_info["end"]},
                "quantizationId": quant_id,
                "artifact": {
                    "uri": f"file:./{f['filename']}",
                    "sha256": sha256,
                    "format": "gguf",
                    "formatVersion": "3",
                    "fileBytes": size,
                },
                "executionProfileId": f"cpu-{quant_id}",
                "peakRuntimeMemoryBytes": size * 3,
                "estimatedKvBytesPerToken": 65536,
                "compatibleBackends": ["llamaCpp"],
                "requiredComputeUnits": ["cpu"],
                "requiredOperatorIds": ["rms_norm", "attention", "ffn"],
                "estimatedPrefillMicroseconds": 8000,
                "estimatedDecodeMicroseconds": 1500,
                "sensitivityScore": 0.8,
            })
        else:
            # Single file — covers all layers
            shards.append({
                "shardId": f"{model_id}-full",
                "stageIds": [f"all-layers-0-{total_layers}"],
                "stageKind": "transformerLayers",
                "layers": {"start": 0, "endExclusive": total_layers},
                "quantizationId": quant_id,
                "artifact": {
                    "uri": f"file:./{f['filename']}",
                    "sha256": sha256,
                    "format": "gguf",
                    "formatVersion": "3",
                    "fileBytes": size,
                },
                "executionProfileId": f"cpu-{quant_id}",
                "peakRuntimeMemoryBytes": size * 3,
                "estimatedKvBytesPerToken": 65536,
                "compatibleBackends": ["llamaCpp"],
                "requiredComputeUnits": ["cpu"],
                "requiredOperatorIds": ["rms_norm", "attention", "ffn", "lm_head"],
                "estimatedPrefillMicroseconds": 15000,
                "estimatedDecodeMicroseconds": 3000,
                "sensitivityScore": 1.0,
            })

    manifest = {
        "modelId": model_id,
        "manifestId": f"{model_id}-v1",
        "version": "1.0.0",
        "totalTransformerLayers": total_layers,
        "quantizations": quantizations,
        "shards": shards,
        "boundaries": [],
        "calibrationSamples": [],
    }

    manifest_path = output_dir / "manifest.json"
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    return manifest_path


def cmd_fetch(args):
    """Download model artifacts and generate manifest."""
    output_dir = Path(args.output).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    model_id = args.model
    files = []
    total_layers = 22

    # Resolve model from registry
    if args.auto or model_id in KNOWN_MODELS:
        if args.auto:
            model_id = "tinyllama-1.1b"  # default for auto
        info = KNOWN_MODELS.get(model_id)
        if not info:
            print(f"Error: unknown model '{model_id}'", file=sys.stderr)
            print(f"Known models: {', '.join(KNOWN_MODELS.keys())}", file=sys.stderr)
            return 1

        print(f"Model: {model_id} — {info['description']}")
        print(f"Source: Hugging Face — {info['hf_repo']}")
        total_layers = info["total_layers"]
        repo = info["hf_repo"]

        for entry in info["files"]:
            local = download_hf(repo, entry["filename"], output_dir)
            files.append({**entry, "_local_path": local})

    elif args.url:
        # Direct URL download
        name = args.name or "downloaded-model"
        filename = f"{name}.gguf"
        local = download_url(args.url, output_dir, filename)
        files.append({"filename": filename, "shard": None, "quantization": "q4_k_m", "_local_path": local})
        total_layers = args.layers or 22
        model_id = name

    else:
        print(f"Error: specify --model, --auto, or --url", file=sys.stderr)
        return 1

    # Generate manifest
    manifest_path = generate_manifest(
        model_id=model_id,
        model_name=model_id,
        files=files,
        total_layers=total_layers,
        output_dir=output_dir,
    )

    print(f"\nDone! Model files in: {output_dir}")
    print(f"Manifest: {manifest_path}")
    print(f"\nTo use this model:")
    print(f"  socrates-engine manifest validate {manifest_path}")
    print(f"  python -c \"from socrates_engine.runtime import Runtime; rt = Runtime(model_root='{output_dir}'); ...\"")
    return 0


def main():
    parser = argparse.ArgumentParser(
        "socrates-fetch",
        description="Download GGUF models from Hugging Face or URL, and generate manifests.",
    )
    parser.add_argument("--model", help="Model ID (e.g., tinyllama-1.1b, phi-2, llama-3.2-3b)")
    parser.add_argument("--auto", action="store_true", help="Auto-detect and download default model")
    parser.add_argument("--url", help="Direct URL to a GGUF file")
    parser.add_argument("--name", help="Model name for URL downloads")
    parser.add_argument("--layers", type=int, help="Total transformer layers (for URL downloads)")
    parser.add_argument("--output", default="./models", help="Output directory")
    parser.add_argument("--list", action="store_true", help="List known models and exit")

    args = parser.parse_args()

    if args.list:
        print("Known models:")
        for mid, info in KNOWN_MODELS.items():
            print(f"  {mid:20s} — {info['description']} ({info['total_layers']} layers)")
        return 0

    return cmd_fetch(args)


if __name__ == "__main__":
    sys.exit(main())
