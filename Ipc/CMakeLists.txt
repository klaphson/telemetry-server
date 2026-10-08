add_library(
    Ipc STATIC
    src/SharedRingBuffer.cpp
)

target_include_directories(
    Ipc
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
    Ipc
    PUBLIC
        Protocol
    PRIVATE
        telemetry_build_options
)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()