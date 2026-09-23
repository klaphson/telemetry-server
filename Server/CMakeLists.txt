add_library(Server STATIC src/Server.cpp)
target_include_directories(Server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(Server PRIVATE telemetry_build_options)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
