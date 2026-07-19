add_executable(multi-node-test multi_node_test.cpp)
target_link_libraries(multi-node-test PRIVATE socrates)
set_project_warnings(multi-node-test)

# Also build the real E2E tests
add_executable(socrates_e2e_tests)
target_sources(socrates_e2e_tests PRIVATE e2e_tests.cpp)
target_link_libraries(socrates_e2e_tests PRIVATE socrates GTest::gtest GTest::gtest_main GTest::gmock)
set_project_warnings(socrates_e2e_tests)
include(GoogleTest)
gtest_discover_tests(socrates_e2e_tests TEST_PREFIX "e2e.")
