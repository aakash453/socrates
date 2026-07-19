add_executable(socrates_integration_tests)

target_sources(socrates_integration_tests PRIVATE
    cluster_tests.cpp
    membership_profiler_tests.cpp
)

if(SOCRATES_HAS_PROTOBUF)
    target_sources(socrates_integration_tests PRIVATE transport_tests.cpp)
endif()

target_link_libraries(socrates_integration_tests PRIVATE
    socrates
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
)

set_project_warnings(socrates_integration_tests)

include(GoogleTest)
gtest_discover_tests(socrates_integration_tests)
