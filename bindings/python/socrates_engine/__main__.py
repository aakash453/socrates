"""
CLI tools for the Socrates Edge AI Runtime.
"""

import argparse
import sys


def cmd_fetch(args):
    """Download model artifacts."""
    print(f"Fetching model: {args.model or 'auto'}")
    print(f"  Output directory: {args.output}")
    print("  (model fetching is a stub — place real model files in the output dir)")
    return 0


def cmd_manifest(args):
    """Validate a model manifest."""
    import json
    import os

    path = args.path
    if not os.path.exists(path):
        print(f"Error: manifest not found: {path}", file=sys.stderr)
        return 1

    with open(path) as f:
        data = json.load(f)

    # Basic validation
    required = ["model_id", "version", "shards"]
    missing = [k for k in required if k not in data]
    if missing:
        print(f"Error: missing required fields: {missing}", file=sys.stderr)
        return 1

    print(f"Manifest OK: model={data['model_id']} version={data['version']}")
    print(f"  Shards: {len(data['shards'])}")
    for s in data["shards"]:
        print(f"    - {s.get('shard_id', '?')}: {s.get('artifact', {}).get('uri', '?')}")
    return 0


def cmd_profile(args):
    """Profile local hardware capabilities."""
    print("Hardware Profile (stub)")
    print(f"  State directory: {args.state_dir}")
    print(f"  (run the native socrates-profiler tool for real profiling)")
    return 0


def main():
    parser = argparse.ArgumentParser(
        "socrates-engine", description="Socrates Edge AI Runtime CLI"
    )
    sub = parser.add_subparsers(dest="command")

    # fetch
    f = sub.add_parser("fetch", help="Download model artifacts")
    f.add_argument("--model", help="Model identifier")
    f.add_argument("--auto", action="store_true", help="Auto-detect model")
    f.add_argument("--output", default="./models", help="Output directory")

    # manifest
    m = sub.add_parser("manifest", help="Validate a model manifest")
    m.add_argument("command_manifest", nargs="?", default="validate",
                   choices=["validate"])
    m.add_argument("path", help="Path to manifest.json")

    # profile
    p = sub.add_parser("profile", help="Profile local hardware")
    p.add_argument("--state-dir", default="/tmp/socrates-state",
                   help="State directory")

    args = parser.parse_args()
    if args.command == "fetch":
        return cmd_fetch(args)
    elif args.command == "manifest":
        return cmd_manifest(args)
    elif args.command == "profile":
        return cmd_profile(args)
    else:
        parser.print_help()
        return 1


if __name__ == "__main__":
    sys.exit(main())
