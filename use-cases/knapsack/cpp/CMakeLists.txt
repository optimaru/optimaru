cmake_minimum_required(VERSION 3.16)
project(knapsack_cpp CXX)

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

add_library(ks_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/construction.cpp
    src/solver.cpp
)
target_include_directories(ks_core PUBLIC include)
target_link_libraries(ks_core PUBLIC optimaru)
target_compile_options(ks_core PRIVATE -Wall -Wextra)

add_executable(ks_demo src/main.cpp)
target_link_libraries(ks_demo PRIVATE ks_core)
target_compile_options(ks_demo PRIVATE -Wall -Wextra)

# Monolithic path: per-constraint hand checks, the greedy construction,
# incremental == full recompute on random moves, ScoreChecks::Full and brute-force
# solves.
add_executable(ks_test test/ks_test.cpp)
target_link_libraries(ks_test PRIVATE ks_core)
target_compile_options(ks_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (ks/constraints.hpp): the same score decomposed into
# one component per constraint on the engine's DecomposedObjective,
# checked bit-for-bit against the monolithic calculators.
add_executable(ks_plugin_test test/ks_plugin_test.cpp)
target_link_libraries(ks_plugin_test PRIVATE ks_core)
target_compile_options(ks_plugin_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME ks_test COMMAND ks_test)
add_test(NAME ks_plugin_test COMMAND ks_plugin_test)
