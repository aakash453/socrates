#!/usr/bin/env bash
set -euo pipefail

# build_xcframework.sh
# Builds SocratesEngine.xcframework for iOS (device + simulator) and macOS.
#
# Usage:
#   ./scripts/build_xcframework.sh                     # both iOS and macOS
#   ./scripts/build_xcframework.sh --ios-only           # iOS only
#   ./scripts/build_xcframework.sh --macos-only         # macOS only
#   ./scripts/build_xcframework.sh --output ./dist      # custom output dir

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="${ROOT_DIR}/build/package"
IOS_ONLY=false
MACOS_ONLY=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --ios-only)
      IOS_ONLY=true
      shift
      ;;
    --macos-only)
      MACOS_ONLY=true
      shift
      ;;
    --help|-h)
      echo "Usage: $0 [--ios-only] [--macos-only] [--output DIR]"
      echo ""
      echo "Builds SocratesEngine.xcframework for iOS (device + simulator) and macOS."
      echo "  --ios-only     Build iOS slices only"
      echo "  --macos-only   Build macOS slice only"
      echo "  --output DIR   Output directory (default: build/package)"
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      exit 1
      ;;
  esac
done

mkdir -p "${OUTPUT_DIR}"

build_ios() {
  echo "=== Building iOS device slice (arm64) ==="
  cmake --preset ios-device-release -S "${ROOT_DIR}"
  cmake --build --preset ios-device-release --parallel

  echo "=== Building iOS simulator slice (arm64) ==="
  cmake --preset ios-simulator-release -S "${ROOT_DIR}"
  cmake --build --preset ios-simulator-release --parallel

  echo "=== Creating iOS XCFramework ==="
  local device_lib="${ROOT_DIR}/build/ios-device-release/Release-iphoneos/libsocrates.a"
  local sim_lib="${ROOT_DIR}/build/ios-simulator-release/Release-iphonesimulator/libsocrates.a"

  # CMake Xcode generator puts libraries in a different path structure
  # Try alternative paths if primary not found
  if [[ ! -f "${device_lib}" ]]; then
    device_lib=$(find "${ROOT_DIR}/build/ios-device-release" -name "libsocrates.a" -path "*-iphoneos*" | head -1)
  fi
  if [[ ! -f "${sim_lib}" ]]; then
    sim_lib=$(find "${ROOT_DIR}/build/ios-simulator-release" -name "libsocrates.a" -path "*-iphonesimulator*" | head -1)
  fi

  if [[ ! -f "${device_lib}" ]] || [[ ! -f "${sim_lib}" ]]; then
    echo "ERROR: Could not find built iOS libraries."
    echo "  Device: ${device_lib:-NOT FOUND}"
    echo "  Simulator: ${sim_lib:-NOT FOUND}"
    exit 1
  fi

  xcodebuild -create-xcframework \
    -library "${device_lib}" \
    -headers "${ROOT_DIR}/include" \
    -library "${sim_lib}" \
    -headers "${ROOT_DIR}/include" \
    -output "${OUTPUT_DIR}/SocratesEngine.xcframework"

  echo "iOS XCFramework created at ${OUTPUT_DIR}/SocratesEngine.xcframework"
}

build_macos() {
  echo "=== Building macOS slice ==="
  cmake --preset macos-release -S "${ROOT_DIR}"
  cmake --build --preset macos-release --parallel

  echo "=== Packaging macOS framework ==="
  cmake --install "${ROOT_DIR}/build/macos-release" --prefix "${OUTPUT_DIR}/macos"
  cpack --config "${ROOT_DIR}/build/macos-release/CPackConfig.cmake" -B "${OUTPUT_DIR}"

  echo "macOS package created at ${OUTPUT_DIR}/"
}

if ${IOS_ONLY}; then
  build_ios
elif ${MACOS_ONLY}; then
  build_macos
else
  build_ios
  build_macos
fi

echo "=== Done ==="
