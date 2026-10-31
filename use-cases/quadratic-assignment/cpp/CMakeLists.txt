cmake_minimum_required(VERSION 3.16)
project(quadratic_assignment_cpp CXX)

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

add_library(qap_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(qap_core PUBLIC include)
target_link_libraries(qap_core PUBLIC optimaru)
target_compile_options(qap_core PRIVATE -Wall -Wextra)

add_executable(qap_demo src/main.cpp)
target_link_libraries(qap_demo PRIVATE qap_core)
target_compile_options(qap_demo PRIVATE -Wall -Wextra)

# Monolithic path: per-constraint hand checks, incremental == full recompute
# on random moves, FULL_ASSERT, warm starts, the bijection guard and
# brute-force solves.
add_executable(qap_test test/qap_test.cpp)
target_link_libraries(qap_test PRIVATE qap_core)
target_compile_options(qap_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (qap/constraints.hpp): the same score decomposed into
# one component per constraint on the engine's DecomposedScoreCalculator,
# checked bit-for-bit against the monolithic calculators.
add_executable(qap_plugin_test test/qap_plugin_test.cpp)
target_link_libraries(qap_plugin_test PRIVATE qap_core)
target_compile_options(qap_plugin_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME qap_test COMMAND qap_test)
add_test(NAME qap_plugin_test COMMAND qap_plugin_test)
