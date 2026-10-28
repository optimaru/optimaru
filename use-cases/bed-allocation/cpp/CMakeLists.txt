cmake_minimum_required(VERSION 3.16)
project(bed_allocation_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The generic engine (header-only), consumed like the Java quickstarts consume
# the ai.timefold.solver dependency.
add_library(optimaru INTERFACE)
target_include_directories(optimaru
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)

add_library(ba_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(ba_core PUBLIC include)
target_link_libraries(ba_core PUBLIC optimaru)
target_compile_options(ba_core PRIVATE -Wall -Wextra)

add_executable(ba_demo src/main.cpp)
target_link_libraries(ba_demo PRIVATE ba_core)
target_compile_options(ba_demo PRIVATE -Wall -Wextra)

add_executable(ba_test test/ba_test.cpp)
target_link_libraries(ba_test PRIVATE ba_core)
target_compile_options(ba_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (ba/constraints.hpp): the same provider decomposed into
# one component each on the engine's DecomposedScoreCalculator, checked
# bit-for-bit against the monolithic calculators.
add_executable(ba_plugin_test test/ba_plugin_test.cpp)
target_link_libraries(ba_plugin_test PRIVATE ba_core)
target_compile_options(ba_plugin_test PRIVATE -Wall -Wextra)

# Engine-only quality bench over the dumped comparison instances
# (benchmarks/bed-allocation/dump_instances.py); used for interleaved A/B tuning runs.
add_executable(ba_quality_bench bench/ba_quality_bench.cpp)
target_link_libraries(ba_quality_bench PRIVATE ba_core)
target_compile_options(ba_quality_bench PRIVATE -Wall -Wextra)
