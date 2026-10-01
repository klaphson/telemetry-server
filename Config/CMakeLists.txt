add_library(
    Config STATIC
    src/ConfigParser.cpp
)

target_include_directories(
    Config
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
    Config
    PRIVATE
        Protocol
        telemetry_build_options
)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()