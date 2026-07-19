option(SOCRATES_ENABLE_TESTS "Build tests" ON)
option(SOCRATES_ENABLE_TOOLS "Build CLI tools" OFF)
option(SOCRATES_ENABLE_QNN "Enable Qualcomm QNN backend (requires QNN SDK)" OFF)
option(SOCRATES_ENABLE_LLAMA "Enable llama.cpp backend" ON)
option(SOCRATES_ENABLE_EXECUTORCH "Enable ExecuTorch backend" ON)
option(SOCRATES_ENABLE_MLX "Enable Apple MLX backend (GPU + Neural Engine)" OFF)
option(SOCRATES_ENABLE_LITERT "Enable LiteRT backend (Windows GPU/NPU via WinML/DirectML)" OFF)
option(SOCRATES_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(SOCRATES_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(SOCRATES_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(SOCRATES_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(SOCRATES_BUILD_BENCHMARKS "Build performance benchmarks" OFF)

if(SOCRATES_ENABLE_ASAN OR SOCRATES_ENABLE_TSAN OR SOCRATES_ENABLE_UBSAN)
    set(SOCRATES_ENABLE_TESTS ON)
endif()

if(SOCRATES_ENABLE_QNN)
    message(STATUS "QNN backend enabled — requires Qualcomm QNN SDK")
endif()

if(SOCRATES_ENABLE_MLX)
    message(STATUS "MLX backend enabled — requires Apple MLX framework")
endif()

if(SOCRATES_ENABLE_LITERT)
    message(STATUS "LiteRT backend enabled — requires Windows AI SDK")
endif()

if(NOT SOCRATES_ENABLE_QNN)
    add_compile_definitions(SOCRATES_QNN_DISABLED)
endif()

if(NOT SOCRATES_ENABLE_LLAMA)
    add_compile_definitions(SOCRATES_LLAMA_DISABLED)
endif()

if(NOT SOCRATES_ENABLE_EXECUTORCH)
    add_compile_definitions(SOCRATES_EXECUTORCH_DISABLED)
endif()

if(NOT SOCRATES_ENABLE_MLX)
    add_compile_definitions(SOCRATES_MLX_DISABLED)
endif()

if(NOT SOCRATES_ENABLE_LITERT)
    add_compile_definitions(SOCRATES_LITERT_DISABLED)
endif()
