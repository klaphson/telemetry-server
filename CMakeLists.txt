cmake_minimum_required(VERSION 3.24)
project(telemetry_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TELEMETRY_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(telemetry_build_options INTERFACE)
target_compile_options(telemetry_build_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)
if(TELEMETRY_ENABLE_SANITIZERS)
    target_compile_options(telemetry_build_options INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(telemetry_build_options INTERFACE -fsanitize=address,undefined)
endif()

include(CTest)

if(BUILD_TESTING)
    find_package(Catch2 3 QUIET)
    if(NOT Catch2_FOUND)
        include(FetchContent)
        FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.8.1
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    endif()
    include(Catch)
endif()

add_subdirectory(Protocol)
add_subdirectory(Server)
add_subdirectory(Reader)

add_executable(telemetry-server src/main.cpp)
target_link_libraries(telemetry-server PRIVATE Server Reader telemetry_build_options)

if(BUILD_TESTING)
    add_test(NAME telemetry_process_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_smoke.sh" $<TARGET_FILE:telemetry-server>)
    add_test(NAME telemetry_graceful_shutdown
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/graceful_shutdown.sh" $<TARGET_FILE:telemetry-server>)

    set_tests_properties(
        telemetry_process_smoke
        telemetry_graceful_shutdown
        PROPERTIES TIMEOUT 15 RESOURCE_LOCK telemetry_tcp_port)
endif()
