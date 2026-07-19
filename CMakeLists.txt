cmake_minimum_required(VERSION 3.22)

# Android NDK toolchain defaults FIND_ROOT_PATH_MODE to ONLY. Pre-set to
# BOTH so Conan cross-compiled packages (spdlog, fmt, etc.) are found.
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH CACHE STRING "")
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH CACHE STRING "")
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH CACHE STRING "")

project(SocratesEngine
    VERSION 0.1.0
    DESCRIPTION "Edge AI Runtime Engine"
    HOMEPAGE_URL "https://github.com/multiverse/socrates"
    LANGUAGES CXX
)

# Force C++20 — overrides Conan's C++17 setting
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Remove the Conan cppstd override flag
if(CMAKE_CXX_FLAGS MATCHES "-std=gnu\\+\\+17")
    string(REGEX REPLACE "-std=gnu\\+\\+17" "-std=c++20" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()
if(CMAKE_CXX_FLAGS MATCHES "-std=gnu17")
    string(REGEX REPLACE "-std=gnu17" "-std=c++20" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# ── Build options (must precede any option-dependent logic) ──────────────────
include(cmake/EdgeAIOptions.cmake)

# ── Compiler warnings ────────────────────────────────────────────────────────
include(cmake/CompilerWarnings.cmake)

# ── Validate toolchain ───────────────────────────────────────────────────────
include(cmake/Dependencies.cmake)

# ── Qualcomm QNN SDK ─────────────────────────────────────────────────────────
if(SOCRATES_ENABLE_QNN)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(QNN QUIET)
    if(QNN_FOUND)
        message(STATUS "QNN SDK version: ${QNN_VERSION}")
    else()
        message(WARNING "QNN SDK not found — QNN backend will not be linked. "
                        "Set QNN_SDK_ROOT to the SDK path.")
    endif()
endif()

# Protobuf & gRPC from Homebrew (optional for MVP builds without transport)
find_package(Protobuf QUIET)
find_package(gRPC QUIET)

set(SOCRATES_HAS_PROTOBUF ${Protobuf_FOUND})
set(SOCRATES_HAS_GRPC ${gRPC_FOUND})

if(Protobuf_FOUND)
    message(STATUS "Protobuf found: ${Protobuf_VERSION}")
else()
    message(STATUS "Protobuf not found — transport subsystem disabled")
endif()

if(gRPC_FOUND)
    message(STATUS "gRPC found: ${gRPC_VERSION}")
else()
    message(STATUS "gRPC not found — transport subsystem disabled")
endif()

# OpenSSL from Homebrew
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
endif()

# Platform-native mDNS
if(APPLE)
    find_library(DNS_SD_LIBRARY dns_sd)
    if(DNS_SD_LIBRARY)
        message(STATUS "Bonjour dns_sd found")
    endif()
elseif(UNIX AND NOT ANDROID)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(AVAHI avahi-client)
    endif()
endif()

# llama.cpp backend — try system install, then FetchContent from GitHub
if(SOCRATES_ENABLE_LLAMA)
    find_path(LLAMA_INCLUDE_DIR llama.h)
    find_library(LLAMA_LIBRARY llama)
    if(LLAMA_INCLUDE_DIR AND LLAMA_LIBRARY)
        message(STATUS "llama.cpp found at ${LLAMA_INCLUDE_DIR}")
        set(SOCRATES_HAS_LLAMA_CPP TRUE)
    else()
        include(FetchContent)
        FetchContent_Declare(
            llama.cpp
            GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
            GIT_TAG        b4730
            GIT_SHALLOW    TRUE
        )
        set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "")
        set(LLAMA_BUILD_TESTS OFF CACHE BOOL "")
        set(LLAMA_BUILD_SERVER OFF CACHE BOOL "")
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "")
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        FetchContent_MakeAvailable(llama.cpp)
        if(TARGET llama)
            set(LLAMA_INCLUDE_DIR ${llama.cpp_SOURCE_DIR})
            set(LLAMA_LIBRARY llama)
            set(SOCRATES_HAS_LLAMA_CPP TRUE)
            message(STATUS "llama.cpp fetched and built from source")
        else()
            message(STATUS "llama.cpp not found, using simulated inference")
        endif()
    endif()
endif()

# ── Dependencies ─────────────────────────────────────────────────────────────
if(ANDROID)
    # Android/iOS: header-only vendored copies. Avoids Conan ABI mismatch.
    set(spdlog_FOUND TRUE)
    set(nlohmann_json_FOUND TRUE)
    set(tl-expected_FOUND TRUE)
    set(SQLite3_FOUND TRUE)
    message(STATUS "Android/iOS: using vendored header-only dependencies")
else()
    find_package(spdlog CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(tl-expected CONFIG REQUIRED)
    find_package(SQLite3 CONFIG REQUIRED)
endif()

if(SOCRATES_ENABLE_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

# ── Main library target ──────────────────────────────────────────────────────
if(ANDROID)
    enable_language(C)
    add_library(socrates SHARED)
else()
    add_library(socrates)
endif()
add_library(Socrates::Engine ALIAS socrates)

target_include_directories(socrates PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

target_compile_features(socrates PUBLIC cxx_std_20)
set_target_properties(socrates PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# Override Conan's C++17 toolchain setting
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(spdlog_FOUND)
    if(ANDROID)
        target_compile_definitions(socrates PUBLIC
            SPDLOG_HEADER_ONLY SPDLOG_FMT_EXTERNAL FMT_HEADER_ONLY)
        target_include_directories(socrates PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/vendor/spdlog/include
            ${CMAKE_CURRENT_SOURCE_DIR}/vendor/fmt/include)
    endif()
endif()
if(nlohmann_json_FOUND)
    if(ANDROID)
        target_include_directories(socrates PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/vendor/nlohmann/include)
    else()
        target_link_libraries(socrates PUBLIC nlohmann_json::nlohmann_json)
    endif()
endif()
if(tl-expected_FOUND)
    if(ANDROID)
        target_include_directories(socrates PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/vendor/tl-expected/include)
    else()
        target_link_libraries(socrates PRIVATE tl::expected)
    endif()
endif()
if(SQLite3_FOUND)
    if(ANDROID)
        # Use vendored amalgamation (compile sqlite3.c into the library)
        target_include_directories(socrates PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite3")
        target_sources(socrates PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite3/sqlite3.c")
        # sqlite3.c is vendored third-party code — suppress its warnings
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite3/sqlite3.c"
            PROPERTIES COMPILE_FLAGS "-w")
        target_include_directories(socrates PRIVATE
            "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/${CMAKE_HOST_SYSTEM_NAME}-x86_64/sysroot/usr/include")
    else()
        target_link_libraries(socrates PUBLIC SQLite::SQLite3)
    endif()
endif()


if(Protobuf_FOUND)
    target_link_libraries(socrates PUBLIC protobuf::libprotobuf)
    target_compile_definitions(socrates PUBLIC SOCRATES_HAS_PROTOBUF=1)
endif()

if(gRPC_FOUND)
    target_link_libraries(socrates PRIVATE gRPC::grpc++)
    target_compile_definitions(socrates PUBLIC SOCRATES_HAS_GRPC=1)
endif()

if(OpenSSL_FOUND)
    target_link_libraries(socrates PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(socrates PUBLIC SOCRATES_HAS_OPENSSL=1)
endif()

# Platform-native mDNS linking
if(APPLE AND DNS_SD_LIBRARY)
    target_link_libraries(socrates PRIVATE ${DNS_SD_LIBRARY})
elseif(UNIX AND NOT ANDROID AND AVAHI_FOUND)
    target_link_libraries(socrates PRIVATE ${AVAHI_LIBRARIES})
    target_include_directories(socrates PRIVATE ${AVAHI_INCLUDE_DIRS})
endif()

# llama.cpp linking
if(SOCRATES_HAS_LLAMA_CPP)
    target_link_libraries(socrates PRIVATE ${LLAMA_LIBRARY})
    target_include_directories(socrates PRIVATE ${LLAMA_INCLUDE_DIR})
    target_compile_definitions(socrates PRIVATE SOCRATES_HAS_LLAMA_CPP=1)
    # Newer llama.cpp requires ggml for backend loading
    find_library(GGML_LIBRARY ggml)
    if(GGML_LIBRARY)
        target_link_libraries(socrates PRIVATE ${GGML_LIBRARY})
        message(STATUS "ggml linked for backend loading")
    endif()
endif()

if(SOCRATES_ENABLE_QNN AND QNN_FOUND)
    target_link_libraries(socrates PRIVATE ${QNN_LIBRARIES})
    target_include_directories(socrates PRIVATE ${QNN_INCLUDE_DIRS})
    target_compile_definitions(socrates PRIVATE SOCRATES_HAS_QNN=1)
endif()

# Architecture-specific optimizations (compile flags)
include(cmake/ArchOptimizations.cmake)

# Apple Metal framework (for MLX backend on Apple Silicon)
if(APPLE AND SOCRATES_ENABLE_MLX)
    find_library(METAL_LIBRARY Metal REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    target_link_libraries(socrates PRIVATE ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

set_project_warnings(socrates)

# ── Subdirectories ───────────────────────────────────────────────────────────
add_subdirectory(src)

# ── C ABI bindings (consumed by JNI, ObjC++, C++/WinRT) ──────────────────────
target_sources(socrates PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bindings/c/socrates.cpp)


if(SOCRATES_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SOCRATES_ENABLE_TOOLS)
    add_subdirectory(tools)
endif()

# ── Install ──────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
include(cmake/Packaging.cmake)

install(TARGETS socrates
    EXPORT SocratesEngineTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# install(EXPORT) skipped — FetchContent targets (llama.cpp) are not
# part of any export set. Re-enable when packaging is needed.
# install(EXPORT SocratesEngineTargets
#     NAMESPACE Socrates::
#     DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SocratesEngine
# )
