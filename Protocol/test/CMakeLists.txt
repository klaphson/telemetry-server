add_executable(
    telemetry_protocol_test
    TelemetryCodecTest.cpp
)

target_link_libraries(
    telemetry_protocol_test
    PRIVATE
        Protocol
        Catch2::Catch2WithMain
        telemetry_build_options
)

catch_discover_tests(
    telemetry_protocol_test
    TEST_PREFIX "TelemetryCodecTest."
    PROPERTIES TIMEOUT 10
)
