if(NOT ANDROID AND NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR
        "No Conan toolchain file detected.
"
        "Run: conan install . --output-folder=build/conan-debug --build=missing "
        "--settings=build_type=Debug
"
        "Then configure with:
"
        "  cmake --preset macos-debug
"
        "Or set CMAKE_TOOLCHAIN_FILE manually."
    )
endif()
