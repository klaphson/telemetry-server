add_library(
    Storage STATIC
    src/FileStore.cpp
)

target_include_directories(
    Storage
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
    Storage
    PRIVATE
        Protocol
        telemetry_build_options
)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()