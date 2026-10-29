cmake_minimum_required(VERSION 3.16)
project(conference_scheduling_cpp CXX)

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

add_library(cs_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(cs_core PUBLIC include)
target_link_libraries(cs_core PUBLIC optimaru)
target_compile_options(cs_core PRIVATE -Wall -Wextra)

add_executable(cs_demo src/main.cpp)
target_link_libraries(cs_demo PRIVATE cs_core)
target_compile_options(cs_demo PRIVATE -Wall -Wextra)

add_executable(cs_test test/cs_test.cpp)
target_link_libraries(cs_test PRIVATE cs_core)
target_compile_options(cs_test PRIVATE -Wall -Wextra)

# Solution-quality benchmark (see bench/quality_bench.cpp): demo + scaled
# instances, best-score trajectory, per-constraint breakdown, moves/s.
add_executable(cs_quality_bench bench/quality_bench.cpp)
target_link_libraries(cs_quality_bench PRIVATE cs_core)
target_compile_options(cs_quality_bench PRIVATE -Wall -Wextra)

# Plugin-assembly path (cs/constraints.hpp): the same 36 provider constraints
# (plus the port's synthetic one) projected into one component each on the
# engine's DecomposedScoreCalculator, checked against the monolithic
# calculators.
add_executable(cs_plugin_test test/cs_plugin_test.cpp)
target_link_libraries(cs_plugin_test PRIVATE cs_core)
target_compile_options(cs_plugin_test PRIVATE -Wall -Wextra)
