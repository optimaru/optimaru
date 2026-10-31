cmake_minimum_required(VERSION 3.16)
project(assembly_line_balancing_cpp CXX)

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

add_library(alb_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/construction.cpp
    src/solver.cpp
)
target_include_directories(alb_core PUBLIC include)
target_link_libraries(alb_core PUBLIC optimaru)
target_compile_options(alb_core PRIVATE -Wall -Wextra)

add_executable(alb_demo src/main.cpp)
target_link_libraries(alb_demo PRIVATE alb_core)
target_compile_options(alb_demo PRIVATE -Wall -Wextra)

# Monolithic path: per-constraint hand checks, the ported construction,
# incremental == full recompute on random moves, FULL_ASSERT solves.
add_executable(alb_test test/alb_test.cpp)
target_link_libraries(alb_test PRIVATE alb_core)
target_compile_options(alb_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (alb/constraints.hpp): the same score decomposed into
# one component per constraint on the engine's DecomposedScoreCalculator,
# checked bit-for-bit against the monolithic calculators.
add_executable(alb_plugin_test test/alb_plugin_test.cpp)
target_link_libraries(alb_plugin_test PRIVATE alb_core)
target_compile_options(alb_plugin_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME alb_test COMMAND alb_test)
add_test(NAME alb_plugin_test COMMAND alb_plugin_test)
