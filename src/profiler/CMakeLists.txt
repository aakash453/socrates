target_sources(socrates PRIVATE capability_profiler.cpp)

target_sources(socrates PRIVATE
    "$<$<PLATFORM_ID:Darwin>:${CMAKE_CURRENT_SOURCE_DIR}/platform/apple_profiler.mm>"
    "$<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/platform/windows_profiler.cpp>"
    "$<$<PLATFORM_ID:Linux>:${CMAKE_CURRENT_SOURCE_DIR}/platform/linux_profiler.cpp>"
    "$<$<PLATFORM_ID:Android>:${CMAKE_CURRENT_SOURCE_DIR}/platform/android_profiler.cpp>"
)

if(APPLE)
    target_compile_options(socrates PRIVATE -fobjc-arc)
    target_link_libraries(socrates PRIVATE
        "-framework Foundation"
        "-framework Metal"
        "-framework IOKit"
    )
endif()

if(WIN32)
    target_link_libraries(socrates PRIVATE dxgi)
endif()
