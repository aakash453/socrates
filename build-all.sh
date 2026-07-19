#!/bin/bash
# build-all.sh — Build Socrates for macOS, iOS, and Android
# Run from: /Users/aakash/Documents/multiverse-hackathon/hackathon-dev/socrates
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo " Socrates — Multi-Platform Build"
echo " Platform: $(uname -m) / $(sw_vers -productName 2>/dev/null || echo macOS)"
echo "============================================"

# ── Check prerequisites ──────────────────────────────────────────────────

command -v cmake >/dev/null 2>&1 || { echo "ERROR: cmake not found. brew install cmake"; exit 1; }
command -v ninja >/dev/null 2>&1 || { echo "ERROR: ninja not found. brew install ninja"; exit 1; }

echo ""
echo "cmake:  $(cmake --version | head -1)"
echo "ninja:  $(ninja --version)"
echo ""

# ── 1. macOS M1 Pro (NATIVE) ────────────────────────────────────────────

echo "============================================"
echo " BUILD 1/3: macOS ARM64 (M1 Pro)"
echo "============================================"

# Install Conan deps if not already done
if [ ! -d build/conan-release ]; then
    echo "Installing dependencies via Conan..."
    conan install . --output-folder=build/conan-release \
        --build=missing -s build_type=Release || {
        echo "Conan failed — trying without (system deps only)..."
        mkdir -p build/conan-release
    }
fi

# Skip Conan toolchain if conan failed
echo "Configuring macOS build..."
cmake --preset macos-release 2>&1 || {
    echo "Preset failed — trying manual configure..."
    cmake -B build/macos-release -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DSOCRATES_ENABLE_TESTS=OFF \
        -DSOCRATES_ENABLE_TOOLS=ON \
        -DSOCRATES_ENABLE_QNN=OFF \
        -DSOCRATES_ENABLE_MLX=ON \
        -DSOCRATES_ENABLE_LLAMA=ON \
        -DSOCRATES_WARNINGS_AS_ERRORS=OFF
}

echo "Building macOS (using all cores)..."
cmake --build build/macos-release --parallel "$(sysctl -n hw.logicalcpu 2>/dev/null || echo 8)" 2>&1

echo ""
echo "Verifying macOS artifacts..."
if [ -f build/macos-release/libsocrates.a ]; then
    echo "  ✅ libsocrates.a — $(ls -lh build/macos-release/libsocrates.a | awk '{print $5}')"
elif [ -f build/macos-release/libsocrates.dylib ]; then
    echo "  ✅ libsocrates.dylib — $(ls -lh build/macos-release/libsocrates.dylib | awk '{print $5}')"
fi

if [ -f build/macos-release/tools/socrates-profiler ]; then
    echo "  ✅ socrates-profiler"
    echo ""
    echo "  Running hardware profile..."
    ./build/macos-release/tools/socrates-profiler 2>&1 || echo "  (profiler had issues — check output above)"
fi

echo ""
echo "macOS build complete."
echo ""

# ── 2. iOS (iPad M1) ────────────────────────────────────────────────────

echo "============================================"
echo " BUILD 2/3: iOS ARM64 (iPad M1)"
echo "============================================"

if ! xcode-select -p >/dev/null 2>&1; then
    echo "  ⚠️  Xcode not found — skipping iOS build"
    echo "  Install Xcode and run: xcode-select --install"
else
    echo "Configuring iOS build..."
    cmake --preset ios-device-release 2>&1 || {
        echo "Preset failed — trying manual configure..."
        cmake -B build/ios-device-release -G Xcode \
            -DCMAKE_SYSTEM_NAME=iOS \
            -DCMAKE_OSX_ARCHITECTURES=arm64 \
            -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
            -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
            -DCMAKE_BUILD_TYPE=Release \
            -DSOCRATES_ENABLE_TESTS=OFF \
            -DSOCRATES_ENABLE_TOOLS=OFF \
            -DSOCRATES_ENABLE_MLX=ON \
            -DSOCRATES_ENABLE_LLAMA=ON \
            -DSOCRATES_WARNINGS_AS_ERRORS=OFF
    }

    echo "Building iOS..."
    cmake --build build/ios-device-release --parallel "$(sysctl -n hw.logicalcpu 2>/dev/null || echo 8)" 2>&1

    echo ""
    echo "Verifying iOS artifacts..."
    find build/ios-device-release -name "*.a" -o -name "*.framework" 2>/dev/null | head -5
    echo ""
    echo "iOS build complete."
fi
echo ""

# ── 3. Android (Snapdragon 8x) ──────────────────────────────────────────

echo "============================================"
echo " BUILD 3/3: Android ARM64 (Snapdragon 8x)"
echo "============================================"

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    # Try common locations
    for ndk in \
        ~/Library/Android/sdk/ndk/27.0.12077973 \
        ~/Library/Android/sdk/ndk/26.3.11579264 \
        ~/Android/Sdk/ndk/27.0.12077973 \
        /opt/android-ndk; do
        if [ -d "$ndk" ]; then
            export ANDROID_NDK_HOME="$ndk"
            break
        fi
    done
fi

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    echo "  ⚠️  Android NDK not found — skipping Android build"
    echo "  Install via Android Studio: SDK Manager → SDK Tools → NDK"
    echo "  Then set: export ANDROID_NDK_HOME=~/Library/Android/sdk/ndk/27.0.12077973"
else
    echo "Android NDK: $ANDROID_NDK_HOME"

    echo "Configuring Android build..."
    cmake --preset android-arm64-release 2>&1 || {
        echo "Preset failed — trying manual configure..."
        cmake -B build/android-arm64-release -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_SYSTEM_NAME=Android \
            -DCMAKE_SYSTEM_VERSION=35 \
            -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a \
            -DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
            -DSOCRATES_ENABLE_TESTS=OFF \
            -DSOCRATES_ENABLE_TOOLS=OFF \
            -DSOCRATES_ENABLE_QNN=ON \
            -DSOCRATES_ENABLE_LLAMA=ON \
            -DSOCRATES_WARNINGS_AS_ERRORS=OFF
    }

    echo "Building Android..."
    cmake --build build/android-arm64-release --parallel "$(sysctl -n hw.logicalcpu 2>/dev/null || echo 8)" 2>&1

    echo ""
    echo "Verifying Android artifacts..."
    if [ -f build/android-arm64-release/libsocrates.so ]; then
        echo "  ✅ libsocrates.so — $(ls -lh build/android-arm64-release/libsocrates.so | awk '{print $5}')"
        file build/android-arm64-release/libsocrates.so 2>/dev/null || true
    fi
    echo ""
    echo "Android build complete."
fi

echo ""
echo "============================================"
echo " BUILD SUMMARY"
echo "============================================"
echo ""
echo "Artifacts:"
echo "  macOS:  build/macos-release/"
ls build/macos-release/libsocrates.* build/macos-release/tools/socrates-profiler 2>/dev/null || echo "  (build may need Conan deps — see errors above)"
echo ""
echo "  iOS:    build/ios-device-release/"
find build/ios-device-release -name "*.a" 2>/dev/null | head -3 || echo "  (Xcode required)"
echo ""
echo "  Android: build/android-arm64-release/"
ls build/android-arm64-release/libsocrates.so 2>/dev/null || echo "  (Android NDK required)"
