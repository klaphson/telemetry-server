cmake_minimum_required(VERSION 3.24)
project(telemetry_server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TELEMETRY_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(TELEMETRY_BUILD_BENCHMARKS "Build load generator" OFF)
option(TELEMETRY_PROFILE_CPU "Keep debug symbols and frame pointers in optimized builds for perf" OFF)

add_library(telemetry_build_options INTERFACE)
target_compile_options(telemetry_build_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)
if(TELEMETRY_PROFILE_CPU)
    target_compile_options(telemetry_build_options INTERFACE -g -fno-omit-frame-pointer)
endif()
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
add_subdirectory(Config)
add_subdirectory(Storage)
add_subdirectory(Ipc)
add_subdirectory(Server)
add_subdirectory(Reader)

find_package(Threads REQUIRED)
add_executable(telemetry-server src/main.cpp)
target_link_libraries(telemetry-server PRIVATE Config Server Reader Ipc telemetry_build_options
    Threads::Threads)

if(TELEMETRY_BUILD_BENCHMARKS)
    add_executable(telemetry-load bench/telemetry_load.cpp)
    target_link_libraries(telemetry-load PRIVATE Protocol telemetry_build_options)

    # Development-only CLI tool: does not link into telemetry-server.
    add_executable(telemetry-profile-cpu bench/profile_cpu.cpp)
    target_link_libraries(telemetry-profile-cpu PRIVATE telemetry_build_options)
endif()

if(BUILD_TESTING)
    # Restart, rotation and bounded write-failure checks use the production FileStore.
    add_executable(telemetry-storage-recovery-test tests/storage_recovery.cpp)
    target_link_libraries(telemetry-storage-recovery-test PRIVATE
        Storage Protocol telemetry_build_options Threads::Threads)
    foreach(recovery_case IN ITEMS restart rotation write-failure)
        add_test(NAME telemetry_storage_recovery_${recovery_case}
            COMMAND $<TARGET_FILE:telemetry-storage-recovery-test> ${recovery_case})
        set_tests_properties(telemetry_storage_recovery_${recovery_case}
            PROPERTIES TIMEOUT 15)
    endforeach()

    # Genuine ENOSPC: only a private or explicitly authorized small tmpfs.
    add_executable(telemetry-storage-enospc-test tests/storage_enospc.cpp)
    target_link_libraries(telemetry-storage-enospc-test PRIVATE
        Storage Protocol telemetry_build_options Threads::Threads)
    add_test(NAME telemetry_storage_recovery_enospc
        COMMAND $<TARGET_FILE:telemetry-storage-enospc-test>)
    set_tests_properties(telemetry_storage_recovery_enospc PROPERTIES
        TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK telemetry_enospc_mount)

    # Process lifetime and fault-injection checks run as an isolated C++20 tool.
    add_executable(telemetry-process-failure-test tests/process_failure.cpp)
    target_link_libraries(telemetry-process-failure-test PRIVATE
        Protocol telemetry_build_options Threads::Threads)
    foreach(failure_case IN ITEMS
            reader-killed parent-killed stopped-reader-timeout
            graceful-process graceful-thread)
        add_test(NAME telemetry_process_failure_${failure_case}
            COMMAND $<TARGET_FILE:telemetry-process-failure-test>
                $<TARGET_FILE:telemetry-server> --scenario ${failure_case})
        set_tests_properties(telemetry_process_failure_${failure_case} PROPERTIES
            TIMEOUT 25 RESOURCE_LOCK telemetry_tcp_port)
    endforeach()

    # C++20 integration coverage for opt-in metrics, process and thread Reader.
    add_executable(telemetry-monitoring-test tests/monitoring_metrics.cpp)
    target_link_libraries(telemetry-monitoring-test PRIVATE
        Protocol telemetry_build_options Threads::Threads)
    foreach(mode IN ITEMS process thread)
        add_test(NAME telemetry_monitoring_${mode}
            COMMAND $<TARGET_FILE:telemetry-monitoring-test>
                $<TARGET_FILE:telemetry-server> ${mode})
        set_tests_properties(telemetry_monitoring_${mode} PROPERTIES
            TIMEOUT 25 RESOURCE_LOCK telemetry_tcp_port)
    endforeach()

    # C++20 integration harness: no Python or Catch2 required.
    add_executable(telemetry-multi-client-test tests/multi_client_tcp.cpp)
    target_link_libraries(telemetry-multi-client-test PRIVATE
        Protocol telemetry_build_options Threads::Threads)

    add_test(NAME telemetry_multi_client_verifier_selftest
        COMMAND $<TARGET_FILE:telemetry-multi-client-test> --self-test)
    add_test(NAME telemetry_multi_client_process
        COMMAND $<TARGET_FILE:telemetry-multi-client-test>
            $<TARGET_FILE:telemetry-server> --mode process)
    add_test(NAME telemetry_multi_client_thread
        COMMAND $<TARGET_FILE:telemetry-multi-client-test>
            $<TARGET_FILE:telemetry-server> --mode thread)
    set_tests_properties(
        telemetry_multi_client_process
        telemetry_multi_client_thread
        PROPERTIES TIMEOUT 45 RESOURCE_LOCK telemetry_tcp_port)

    if(TELEMETRY_BUILD_BENCHMARKS)
        add_test(NAME telemetry_cpp_cpu_profiler_selftest
            COMMAND $<TARGET_FILE:telemetry-profile-cpu> --self-test)
    endif()

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME telemetry_benchmark_harness
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/bench" -p "test_*.py")
        set_tests_properties(telemetry_benchmark_harness PROPERTIES TIMEOUT 15)

    endif()

    add_test(NAME telemetry_process_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_smoke.sh" $<TARGET_FILE:telemetry-server>)
    add_test(NAME telemetry_graceful_shutdown
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/graceful_shutdown.sh" $<TARGET_FILE:telemetry-server>)

    add_test(NAME telemetry_threaded_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/threaded_smoke.sh" $<TARGET_FILE:telemetry-server>)

    set_tests_properties(
        telemetry_process_smoke
        telemetry_graceful_shutdown
        telemetry_threaded_smoke
        PROPERTIES TIMEOUT 15 RESOURCE_LOCK telemetry_tcp_port)
endif()
