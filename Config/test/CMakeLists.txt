add_executable(
    ConfigParserTest
    ConfigParserTest.cpp
)

target_link_libraries(
    ConfigParserTest
    PRIVATE
        Config
        Catch2::Catch2WithMain
        telemetry_build_options
)

catch_discover_tests(
    ConfigParserTest
    TEST_PREFIX "ConfigParserTest."
    PROPERTIES TIMEOUT 10
)