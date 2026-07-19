target_sources(socrates PRIVATE
    backend_registry.cpp
    llama_backend.cpp
    executorch_qnn_backend.cpp
)

if(SOCRATES_ENABLE_MLX)
    target_sources(socrates PRIVATE mlx_backend.cpp)
endif()

if(SOCRATES_ENABLE_LITERT)
    target_sources(socrates PRIVATE litert_backend.cpp)
endif()
