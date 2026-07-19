#!/usr/bin/env bash
set -euo pipefail

# build_android.sh
# Builds SocratesEngine for Android: .aar library + optional .apk sample.
#
# Prerequisites:
#   ANDROID_HOME     — path to Android SDK
#   ANDROID_NDK_HOME — path to Android NDK 27.1+
#   QNN_SDK_ROOT     — (optional) Qualcomm QNN SDK for Snapdragon NPU
#
# Usage:
#   ./scripts/build_android.sh                      # .aar only
#   ./scripts/build_android.sh --apk                # .aar + debug .apk
#   ./scripts/build_android.sh --release            # .aar + signed release .apk
#   ./scripts/build_android.sh --output ./dist      # custom output dir

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="${ROOT_DIR}/build/android-package"
BUILD_APK=false
RELEASE=false
QNN_ENABLED=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --apk)
      BUILD_APK=true
      shift
      ;;
    --release)
      RELEASE=true
      BUILD_APK=true
      shift
      ;;
    --qnn)
      QNN_ENABLED=true
      shift
      ;;
    --help|-h)
      echo "Usage: $0 [--apk] [--release] [--qnn] [--output DIR]"
      echo ""
      echo "  --apk       Build sample .apk alongside .aar"
      echo "  --release   Build release .apk (requires keystore)"
      echo "  --qnn       Enable QNN NPU backend (requires QNN_SDK_ROOT)"
      echo "  --output    Output directory (default: build/android-package)"
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      exit 1
      ;;
  esac
done

# Validate environment
if [[ -z "${ANDROID_HOME:-}" ]]; then
  echo "ERROR: ANDROID_HOME is not set. Install Android SDK first."
  echo "  brew install --cask android-commandlinetools"
  echo '  export ANDROID_HOME=$HOME/Library/Android/sdk'
  exit 1
fi

if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
  if [[ -d "${ANDROID_HOME}/ndk" ]]; then
    ANDROID_NDK_HOME=$(ls -d "${ANDROID_HOME}/ndk/"* 2>/dev/null | sort -V | tail -1)
    if [[ -n "${ANDROID_NDK_HOME}" ]]; then
      echo "Auto-detected NDK: ${ANDROID_NDK_HOME}"
      export ANDROID_NDK_HOME
    fi
  fi
  if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    echo "ERROR: ANDROID_NDK_HOME is not set."
    echo "  sdkmanager --install 'ndk;27.1.12297006'"
    echo '  export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/27.1.12297006'
    exit 1
  fi
fi

if ${QNN_ENABLED} && [[ -z "${QNN_SDK_ROOT:-}" ]]; then
  echo "WARNING: --qnn specified but QNN_SDK_ROOT is not set. QNN will be disabled."
  echo "  Download from https://qpm.qualcomm.com/"
  QNN_ENABLED=false
fi

echo "=== Socrates Android Build ==="
echo "ANDROID_HOME:    ${ANDROID_HOME}"
echo "ANDROID_NDK_HOME: ${ANDROID_NDK_HOME}"
echo "QNN enabled:     ${QNN_ENABLED}"
echo "Build APK:       ${BUILD_APK}"
echo ""

# Install Conan dependencies for Android
echo "--- Installing Conan dependencies (Android arm64-v8a) ---"
conan install "${ROOT_DIR}" \
  --output-folder="${ROOT_DIR}/build/conan-android-release" \
  --build=missing \
  --settings=os=Android \
  --settings=os.apilevel=35 \
  --settings=arch=armv8 \
  --settings=compiler=clang \
  --settings=compiler.version=18 \
  --settings=build_type=Release

# Build with Gradle
echo "--- Building with Gradle ---"
cd "${ROOT_DIR}"

GRADLE_ARGS=(
  "-Pandroid.injected.invoked.from.ide=false"
  "-Pandroid.native.buildOutput=verbose"
  "-DANDROID_NDK_HOME=${ANDROID_NDK_HOME}"
)

if ${QNN_ENABLED}; then
  GRADLE_ARGS+=("-PQNN_SDK_ROOT=${QNN_SDK_ROOT}")
fi

# Build .aar
./gradlew :socrates:assembleRelease "${GRADLE_ARGS[@]}"

# Copy .aar to output
mkdir -p "${OUTPUT_DIR}"
AAR_FILE=$(find "${ROOT_DIR}/platform/android/build/outputs/aar" -name "*.aar" -type f | head -1)
if [[ -n "${AAR_FILE}" ]]; then
  cp "${AAR_FILE}" "${OUTPUT_DIR}/"
  echo "AAR output: ${OUTPUT_DIR}/$(basename "${AAR_FILE}")"
fi

# Build .apk if requested
if ${BUILD_APK}; then
  if ${RELEASE}; then
    ./gradlew :app:assembleRelease "${GRADLE_ARGS[@]}"
    APK_FILE=$(find "${ROOT_DIR}/platform/android/app/build/outputs/apk/release" -name "*.apk" -type f | head -1)
  else
    ./gradlew :app:assembleDebug "${GRADLE_ARGS[@]}"
    APK_FILE=$(find "${ROOT_DIR}/platform/android/app/build/outputs/apk/debug" -name "*.apk" -type f | head -1)
  fi
  if [[ -n "${APK_FILE}" ]]; then
    cp "${APK_FILE}" "${OUTPUT_DIR}/"
    echo "APK output: ${OUTPUT_DIR}/$(basename "${APK_FILE}")"
  fi
fi

echo ""
echo "=== Build complete ==="
echo "Output: ${OUTPUT_DIR}"
ls -la "${OUTPUT_DIR}"
