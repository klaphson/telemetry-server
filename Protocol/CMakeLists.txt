add_library(
    Protocol STATIC
    src/TelemetryCodec.cpp
)

target_include_directories(
    Protocol
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
    Protocol
    PRIVATE
        telemetry_build_options
)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
