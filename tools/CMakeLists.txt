add_executable(socrates-profiler)
target_sources(socrates-profiler PRIVATE profiler.cpp)
target_link_libraries(socrates-profiler PRIVATE socrates)
set_project_warnings(socrates-profiler)

add_executable(socrates-manifest-validator)
target_sources(socrates-manifest-validator PRIVATE manifest_validator.cpp)
target_link_libraries(socrates-manifest-validator PRIVATE socrates)
set_project_warnings(socrates-manifest-validator)

add_executable(socrates-trace-replay)
target_sources(socrates-trace-replay PRIVATE trace_replay.cpp)
target_link_libraries(socrates-trace-replay PRIVATE socrates)
set_project_warnings(socrates-trace-replay)

add_executable(socrates-worker)
target_sources(socrates-worker PRIVATE worker_cli.cpp)
target_link_libraries(socrates-worker PRIVATE socrates)
target_include_directories(socrates-worker PRIVATE ${CMAKE_SOURCE_DIR}/bindings/c)
set_project_warnings(socrates-worker)

add_executable(socrates-master)
target_sources(socrates-master PRIVATE master_cli.cpp)
target_link_libraries(socrates-master PRIVATE socrates)
target_include_directories(socrates-master PRIVATE ${CMAKE_SOURCE_DIR}/bindings/c)
# set_project_warnings(socrates-master)  -- raw sockets, sign warnings expected
