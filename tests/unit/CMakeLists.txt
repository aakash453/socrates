add_executable(socrates_unit_tests)

target_sources(socrates_unit_tests PRIVATE
    result_tests.cpp
    types_tests.cpp
    discovery_tests.cpp
)

add_subdirectory(discovery)
add_subdirectory(security)
add_subdirectory(scheduler)
add_subdirectory(runtime)

set_project_warnings(socrates_unit_tests)

target_link_libraries(socrates_unit_tests PRIVATE
    socrates
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
)

include(GoogleTest)
gtest_discover_tests(socrates_unit_tests TEST_PREFIX "unit.")
