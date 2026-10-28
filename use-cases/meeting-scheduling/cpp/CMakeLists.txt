cmake_minimum_required(VERSION 3.16)
project(meeting_scheduling_cpp CXX)

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

add_library(ms_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(ms_core PUBLIC include)
target_link_libraries(ms_core PUBLIC optimaru)
target_compile_options(ms_core PRIVATE -Wall -Wextra)

add_executable(ms_demo src/main.cpp)
target_link_libraries(ms_demo PRIVATE ms_core)
target_compile_options(ms_demo PRIVATE -Wall -Wextra)

add_executable(ms_test test/ms_test.cpp)
target_link_libraries(ms_test PRIVATE ms_core)
target_compile_options(ms_test PRIVATE -Wall -Wextra)

# Quality-benchmark driver (demo + deterministic scaled instances, knob
# overrides for A/B sweeps). Not part of the demo or the test gate.
add_executable(ms_quality_bench bench/ms_quality_bench.cpp)
target_link_libraries(ms_quality_bench PRIVATE ms_core)
target_compile_options(ms_quality_bench PRIVATE -Wall -Wextra)

# Plugin-assembly path (ms/constraints.hpp): the same provider decomposed into
# one component each on the engine's DecomposedScoreCalculator, checked
# bit-for-bit against the monolithic calculators.
add_executable(ms_plugin_test test/ms_plugin_test.cpp)
target_link_libraries(ms_plugin_test PRIVATE ms_core)
target_compile_options(ms_plugin_test PRIVATE -Wall -Wextra)
