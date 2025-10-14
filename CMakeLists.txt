cmake_minimum_required(VERSION 3.16)
project(fixture-runner)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# SDK path - check if SDK is installed or needs to be built from source
if(EXISTS "/usr/local/lib/libkuksa_cpp.a")
    # Production mode - SDK is pre-installed (Docker)
    message(STATUS "Using pre-installed SDK from /usr/local")
    set(SDK_INCLUDE_DIR /usr/local/include)
    # Import the pre-installed library
    add_library(kuksa_cpp STATIC IMPORTED)
    set_target_properties(kuksa_cpp PROPERTIES
        IMPORTED_LOCATION /usr/local/lib/libkuksa_cpp.a
        INTERFACE_INCLUDE_DIRECTORIES /usr/local/include
    )
else()
    # Development mode - build SDK from source
    if(NOT DEFINED SDK_PATH)
        # Check local path first
        set(LOCAL_SDK_PATH "$ENV{HOME}/tr-sdv-sandbox/libkuksa-cpp")
        if(EXISTS "${LOCAL_SDK_PATH}/CMakeLists.txt")
            set(SDK_PATH "${LOCAL_SDK_PATH}")
            message(STATUS "Found local SDK at: ${SDK_PATH}")
        else()
            # Download from GitHub if not found locally
            message(STATUS "Local SDK not found, downloading from GitHub")
            include(FetchContent)
            FetchContent_Declare(
                libkuksa_cpp
                GIT_REPOSITORY https://github.com/tr-sdv-sandbox/libkuksa-cpp.git
                GIT_TAG main
            )
            FetchContent_MakeAvailable(libkuksa_cpp)
            set(SDK_PATH "${libkuksa_cpp_SOURCE_DIR}")
        endif()
    endif()
    message(STATUS "Building SDK from source: ${SDK_PATH}")
    set(WITH_STATE_MACHINE OFF CACHE BOOL "Disable state machine" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "Disable SDK tests" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "Disable SDK examples" FORCE)
    add_subdirectory(${SDK_PATH} ${CMAKE_CURRENT_BINARY_DIR}/sdk)
    set(SDK_INCLUDE_DIR ${SDK_PATH}/include)
endif()

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
        kuksa_cpp
        gRPC::grpc++
        protobuf::libprotobuf
        Threads::Threads
        glog
        nlohmann_json::nlohmann_json
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
