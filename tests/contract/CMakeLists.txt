add_executable(socrates_contract_tests)

target_sources(socrates_contract_tests PRIVATE
    manifest_tests.cpp
    protobuf_tests.cpp
)

target_link_libraries(socrates_contract_tests PRIVATE
    socrates
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
)

set_project_warnings(socrates_contract_tests)

include(GoogleTest)
gtest_discover_tests(socrates_contract_tests)
