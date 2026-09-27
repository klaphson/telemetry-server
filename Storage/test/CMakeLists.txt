add_executable(
    FileStoreTest
    FileStoreTest.cpp
)

target_link_libraries(
    FileStoreTest
    PRIVATE
        Storage
        Protocol
        Catch2::Catch2WithMain
        telemetry_build_options
)

catch_discover_tests(
    FileStoreTest
    TEST_PREFIX "FileStoreTest."
    PROPERTIES TIMEOUT 10
)