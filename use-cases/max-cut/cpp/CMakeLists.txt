cmake_minimum_required(VERSION 3.16)
project(max_cut_cpp CXX)

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

add_library(mc_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(mc_core PUBLIC include)
target_link_libraries(mc_core PUBLIC optimaru)
target_compile_options(mc_core PRIVATE -Wall -Wextra)

add_executable(mc_demo src/main.cpp)
target_link_libraries(mc_demo PRIVATE mc_core)
target_compile_options(mc_demo PRIVATE -Wall -Wextra)

# Monolithic path: per-constraint hand checks, the start partition,
# incremental == full recompute on random moves, ScoreChecks::Full, exact-size,
# pinning, zero-budget and brute-force solves.
add_executable(mc_test test/mc_test.cpp)
target_link_libraries(mc_test PRIVATE mc_core)
target_compile_options(mc_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (mc/constraints.hpp): the same score decomposed into
# one component per constraint on the engine's DecomposedObjective,
# checked bit-for-bit against the monolithic calculators.
add_executable(mc_plugin_test test/mc_plugin_test.cpp)
target_link_libraries(mc_plugin_test PRIVATE mc_core)
target_compile_options(mc_plugin_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME mc_test COMMAND mc_test)
add_test(NAME mc_plugin_test COMMAND mc_plugin_test)
