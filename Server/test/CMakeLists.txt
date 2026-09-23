add_executable(IpcQueueTest IpcQueueTest.cpp)
target_include_directories(IpcQueueTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(IpcQueueTest PRIVATE Catch2::Catch2WithMain telemetry_build_options)
catch_discover_tests(IpcQueueTest TEST_PREFIX "IpcQueueTest." PROPERTIES TIMEOUT 10)

add_executable(ServerBackpressureTest ServerBackpressureTest.cpp)
target_link_libraries(ServerBackpressureTest PRIVATE
    Server Catch2::Catch2WithMain telemetry_build_options)
catch_discover_tests(ServerBackpressureTest
    TEST_PREFIX "ServerBackpressureTest." PROPERTIES TIMEOUT 10)
