add_library(Reader STATIC src/Reader.cpp)
target_include_directories(Reader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(Reader PRIVATE Protocol telemetry_build_options)
