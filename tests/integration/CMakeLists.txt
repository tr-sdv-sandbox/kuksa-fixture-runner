# Integration tests for fixture-runner

# Find GTest
find_package(GTest REQUIRED)

# Integration test executable
add_executable(test_fixture_runner
    test_fixture_runner.cpp
)

# Pass build directory to test so it can find the fixture-runner binary
target_compile_definitions(test_fixture_runner PRIVATE
    BUILD_DIR="${CMAKE_BINARY_DIR}"
)

target_include_directories(test_fixture_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SDK_INCLUDE_DIR}
)

target_link_libraries(test_fixture_runner
    PRIVATE
        kuksa_cpp
        GTest::gtest
        GTest::gtest_main
        gRPC::grpc++
        protobuf::libprotobuf
        Threads::Threads
        glog
        nlohmann_json::nlohmann_json
)

# Register test with CTest
gtest_discover_tests(test_fixture_runner)
