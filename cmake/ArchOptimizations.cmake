# cmake/ArchOptimizations.cmake
# Architecture-specific compiler optimization flags.
#
# Snapdragon 8x (ARM Cortex-X4/A720) — QNN NPU with CPU fallback
# Apple M1/M2/M3/M4 — Metal GPU + Neural Engine with CPU fallback
# Snapdragon X Elite (Windows ARM64) — QNN NPU with CPU fallback

# ── Detect host/target architecture ────────────────────────────────────────

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(SOCRATES_ARCH_ARM64 TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(SOCRATES_ARCH_X86_64 TRUE)
endif()

# ── Apple Silicon (macOS / iOS) ────────────────────────────────────────────

if(APPLE AND SOCRATES_ARCH_ARM64)
    message(STATUS "Target: Apple Silicon (M-series / A-series)")

    add_compile_options(
        -mcpu=apple-m1
        -mtune=apple-m1
        -march=armv8.5-a+fp16+rcpc+dotprod+ssbs
    )

    if(SOCRATES_ENABLE_MLX)
        message(STATUS "  + MLX backend (Metal GPU + ANE)")
    endif()

# ── Android ARM64 (Snapdragon 8 Gen 3) ─────────────────────────────────────

elseif(ANDROID AND SOCRATES_ARCH_ARM64)
    message(STATUS "Target: Android ARM64 (Snapdragon / Exynos / Dimensity)")

    add_compile_options(
        -march=armv8.2-a+dotprod+fp16+rcpc
        -mtune=cortex-a76.cortex-a55
        -ftree-vectorize
        -ftree-slp-vectorize
    )

    if(SOCRATES_ENABLE_QNN)
        message(STATUS "  + Qualcomm QNN backend (Hexagon NPU)")
    endif()

# ── Windows ARM64 (Snapdragon X Elite) ────────────────────────────────────

elseif(WIN32 AND SOCRATES_ARCH_ARM64)
    message(STATUS "Target: Windows ARM64 (Snapdragon X Elite)")

    add_compile_options(/arch:ARMv8.2 /fp:fast)

    if(SOCRATES_ENABLE_QNN)
        message(STATUS "  + Qualcomm QNN backend (Hexagon NPU)")
    endif()
    if(SOCRATES_ENABLE_LITERT)
        message(STATUS "  + LiteRT backend (DirectML GPU)")
    endif()

# ── Linux ARM64 ────────────────────────────────────────────────────────────

elseif(UNIX AND NOT APPLE AND SOCRATES_ARCH_ARM64)
    message(STATUS "Target: Linux ARM64")
    add_compile_options(-march=armv8.2-a+dotprod+fp16 -ftree-vectorize)

# ── x86_64 ─────────────────────────────────────────────────────────────────

elseif(SOCRATES_ARCH_X86_64)
    message(STATUS "Target: x86_64")

    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
    check_cxx_compiler_flag("-mfma" COMPILER_SUPPORTS_FMA)
    if(COMPILER_SUPPORTS_AVX2)
        add_compile_options(-mavx2)
    endif()
    if(COMPILER_SUPPORTS_FMA)
        add_compile_options(-mfma)
    endif()
endif()

# ── Common release optimizations ───────────────────────────────────────────

if(CMAKE_BUILD_TYPE MATCHES "Release")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        add_compile_options(-O3 -fno-math-errno)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        add_compile_options(-O3 -fno-math-errno)
    elseif(MSVC)
        add_compile_options(/O2 /GL /GS-)
    endif()
endif()

# Debug: keep frame pointers for profiling
if(CMAKE_BUILD_TYPE MATCHES "Debug")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
        add_compile_options(-fno-omit-frame-pointer)
    endif()
endif()
