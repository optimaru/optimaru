cmake_minimum_required(VERSION 3.16)
project(tournament_scheduling_cpp CXX)

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

add_library(ts_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(ts_core PUBLIC include)
target_link_libraries(ts_core PUBLIC optimaru)
target_compile_options(ts_core PRIVATE -Wall -Wextra)

add_executable(ts_demo src/main.cpp)
target_link_libraries(ts_demo PRIVATE ts_core)
target_compile_options(ts_demo PRIVATE -Wall -Wextra)

add_executable(ts_test test/ts_test.cpp)
target_link_libraries(ts_test PRIVATE ts_core)
target_compile_options(ts_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (ts/constraints.hpp): the same provider decomposed into
# one component each on the engine's DecomposedScoreCalculator, checked
# bit-for-bit against the monolithic calculators.
add_executable(ts_plugin_test test/ts_plugin_test.cpp)
target_link_libraries(ts_plugin_test PRIVATE ts_core)
target_compile_options(ts_plugin_test PRIVATE -Wall -Wextra)
