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

add_subdirectory(Server)
add_subdirectory(Reader)

add_executable(telemetry-server src/main.cpp)
target_link_libraries(telemetry-server PRIVATE Server Reader telemetry_build_options)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME telemetry_process_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_smoke.sh" $<TARGET_FILE:telemetry-server>)
    set_tests_properties(telemetry_process_smoke PROPERTIES TIMEOUT 15)
endif()
