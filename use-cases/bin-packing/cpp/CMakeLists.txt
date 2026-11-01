cmake_minimum_required(VERSION 3.16)
project(bin_packing_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The generic engine (header-only), consumed as an external
# header-only dependency.
add_library(optimaru INTERFACE)
target_include_directories(optimaru
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)

find_package(Threads REQUIRED)

add_library(bp_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/construction.cpp
    src/solver.cpp
)
target_include_directories(bp_core PUBLIC include)
target_link_libraries(bp_core PUBLIC optimaru Threads::Threads)
target_compile_options(bp_core PRIVATE -Wall -Wextra)

add_executable(bp_demo src/main.cpp)
target_link_libraries(bp_demo PRIVATE bp_core)
target_compile_options(bp_demo PRIVATE -Wall -Wextra)

add_executable(bp_test test/bp_test.cpp)
target_link_libraries(bp_test PRIVATE bp_core)
target_compile_options(bp_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (bp/constraints.hpp): the same score decomposed into
# one component per constraint on the engine's DecomposedScoreCalculator,
# checked bit-for-bit against the monolithic calculators.
add_executable(bp_plugin_test test/bp_plugin_test.cpp)
target_link_libraries(bp_plugin_test PRIVATE bp_core)
target_compile_options(bp_plugin_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME bp_test COMMAND bp_test)
add_test(NAME bp_plugin_test COMMAND bp_plugin_test)
