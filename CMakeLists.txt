cmake_minimum_required(VERSION 3.24)
project(telemetry_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TELEMETRY_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_executable(
    telemetry-server
    src/server_main.cpp
)

add_executable(
    telemetry-writer
    src/writer_main.cpp
)

target_include_directories(telemetry-server PRIVATE include)

target_compile_options(telemetry-server PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)

target_include_directories(telemetry-writer PRIVATE include)

target_compile_options(telemetry-writer PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)

if(TELEMETRY_ENABLE_SANITIZERS)
    target_compile_options(telemetry-server PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_options(telemetry-writer PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(telemetry-server PRIVATE -fsanitize=address,undefined)
    target_link_options(telemetry-writer PRIVATE -fsanitize=address,undefined)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME telemetry_process_smoke COMMAND telemetry-server telemetry-writer --self-test)
endif()
