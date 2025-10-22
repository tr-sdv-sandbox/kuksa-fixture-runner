cmake_minimum_required(VERSION 3.16)
project(fixture-runner)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(glog REQUIRED)
find_package(yaml-cpp REQUIRED)

# Try to find libvss-types first (shared dependency)
find_package(vss-types QUIET)
if(NOT vss-types_FOUND)
    # Check parent directory for development
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../libvss-types/CMakeLists.txt")
        message(STATUS "Found libvss-types in parent directory")
        add_subdirectory(../libvss-types ${CMAKE_BINARY_DIR}/libvss-types)
    else()
        message(STATUS "libvss-types not found, will be fetched by dependencies")
    endif()
endif()

# Find or add libvssdag
find_package(vssdag QUIET)
if(NOT vssdag_FOUND)
    # Check parent directory for development
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../libvssdag/CMakeLists.txt")
        message(STATUS "Using libvssdag from parent directory")
        set(BUILD_EXAMPLES OFF CACHE BOOL "Don't build vssdag examples" FORCE)
        set(BUILD_TESTS OFF CACHE BOOL "Don't build vssdag tests" FORCE)
        set(BUILD_INTEGRATION_TESTS OFF CACHE BOOL "Don't build vssdag integration tests" FORCE)
        add_subdirectory(../libvssdag ${CMAKE_BINARY_DIR}/libvssdag)
    else()
        message(FATAL_ERROR "libvssdag not found. Please install it or place it in parent directory.")
    endif()
endif()

# Find or add libkuksa-cpp
find_package(kuksa_cpp QUIET)
if(NOT kuksa_cpp_FOUND)
    # Check parent directory for development
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../libkuksa-cpp/CMakeLists.txt")
        message(STATUS "Using libkuksa-cpp from parent directory")
        set(BUILD_EXAMPLES OFF CACHE BOOL "Don't build kuksa-cpp examples" FORCE)
        set(BUILD_TESTS OFF CACHE BOOL "Don't build kuksa-cpp tests" FORCE)
        add_subdirectory(../libkuksa-cpp ${CMAKE_BINARY_DIR}/libkuksa-cpp)
    else()
        message(FATAL_ERROR "libkuksa-cpp not found. Please install it or place it in parent directory.")
    endif()
endif()

set(SDK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../libkuksa-cpp/include)

# JSON library - prefer system package, fallback to FetchContent
find_package(nlohmann_json 3.11.0 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "System nlohmann_json not found, using FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(json)
else()
    message(STATUS "Using system nlohmann_json")
endif()

# Fixture runner executable
add_executable(fixture-runner
    src/fixture_runner.cpp
)

# Include directories
target_include_directories(fixture-runner PRIVATE
    ${SDK_INCLUDE_DIR}
)

# Link libraries
target_link_libraries(fixture-runner
    PRIVATE
        kuksa::cpp
        vss::dag
        gRPC::grpc++
        protobuf::libprotobuf
        Threads::Threads
        glog::glog
        yaml-cpp
)

# Add tests
option(BUILD_FIXTURE_RUNNER_TESTS "Build fixture-runner integration tests" ON)
if(BUILD_FIXTURE_RUNNER_TESTS)
    message(STATUS "BUILD_FIXTURE_RUNNER_TESTS is ON, adding tests/integration")
    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests/integration)
else()
    message(STATUS "BUILD_FIXTURE_RUNNER_TESTS is OFF, skipping tests")
endif()
