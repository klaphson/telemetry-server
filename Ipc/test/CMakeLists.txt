add_executable(
    SharedRingBufferTest
    SharedRingBufferTest.cpp
)

target_link_libraries(
    SharedRingBufferTest
    PRIVATE
        Ipc
        Catch2::Catch2WithMain
        telemetry_build_options
)

catch_discover_tests(
    SharedRingBufferTest
    TEST_PREFIX "SharedRingBufferTest."
)