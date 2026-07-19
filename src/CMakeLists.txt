# Source files are added incrementally by implementation tasks.

# Core subsystems (no transport dependency)
add_subdirectory(discovery)
add_subdirectory(security)
add_subdirectory(cluster)
add_subdirectory(model)
add_subdirectory(scheduler)
add_subdirectory(persistence)
add_subdirectory(profiler)
add_subdirectory(inference)
add_subdirectory(pipeline)
add_subdirectory(trace)
add_subdirectory(runtime)

# Transport requires Protobuf and optionally gRPC/OpenSSL
if(SOCRATES_HAS_PROTOBUF)
    add_subdirectory(transport)
endif()

# tensor_codec is always compiled (pipeline uses it for hidden-state serialization)
if(NOT SOCRATES_HAS_PROTOBUF)
    target_sources(socrates PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/transport/tensor_codec.cpp)
endif()
