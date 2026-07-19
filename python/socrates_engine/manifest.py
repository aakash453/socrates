"""socrates-manifest CLI — validate model manifests."""

import argparse
import json
import os
import sys


def main():
    parser = argparse.ArgumentParser("socrates-manifest")
    sub = parser.add_subparsers(dest="command")
    v = sub.add_parser("validate")
    v.add_argument("path", help="Path to manifest.json")
    args = parser.parse_args()

    if args.command == "validate":
        path = args.path
        if not os.path.exists(path):
            print(f"Error: manifest not found: {path}", file=sys.stderr)
            return 1
        with open(path) as f:
            data = json.load(f)
        required = ["model_id", "version", "shards"]
        missing = [k for k in required if k not in data]
        if missing:
            print(f"Error: missing fields: {missing}", file=sys.stderr)
            return 1
        print(f"Manifest OK: model={data['model_id']} v{data['version']}")
        return 0

    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
