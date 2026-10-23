add_executable(BufferTest BufferTest.cpp)
target_include_directories(BufferTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(BufferTest PRIVATE Catch2::Catch2WithMain telemetry_build_options)
catch_discover_tests(BufferTest TEST_PREFIX "BufferTest." PROPERTIES TIMEOUT 10)

add_executable(ServerBackpressureTest ServerBackpressureTest.cpp)
target_link_libraries(ServerBackpressureTest PRIVATE
    Server Protocol Ipc Catch2::Catch2WithMain telemetry_build_options)
catch_discover_tests(ServerBackpressureTest
    TEST_PREFIX "ServerBackpressureTest." PROPERTIES TIMEOUT 10)

add_executable(ServerSignalTest ServerSignalTest.cpp)
target_link_libraries(ServerSignalTest PRIVATE
    Server Catch2::Catch2WithMain telemetry_build_options)
catch_discover_tests(ServerSignalTest
    TEST_PREFIX "ServerSignalTest." PROPERTIES TIMEOUT 10)
