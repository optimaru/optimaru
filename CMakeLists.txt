cmake_minimum_required(VERSION 3.16)
project(optimaru CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(optimaru INTERFACE)
add_library(optimaru::optimaru ALIAS optimaru)
target_include_directories(optimaru INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(optimaru INTERFACE cxx_std_17)

# Tests only when built standalone, not when pulled in via add_subdirectory.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(engine_test test/engine_test.cpp)
    target_link_libraries(engine_test PRIVATE optimaru)
    target_compile_options(engine_test PRIVATE -Wall -Wextra)

    find_package(Threads REQUIRED)
    target_link_libraries(engine_test PRIVATE Threads::Threads)

    enable_testing()
    add_test(NAME engine_test COMMAND engine_test)
endif()
