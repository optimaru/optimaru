cmake_minimum_required(VERSION 3.16)
project(task_assigning_cpp CXX)

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

add_library(ta_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(ta_core PUBLIC include)
target_link_libraries(ta_core PUBLIC optimaru)
target_compile_options(ta_core PRIVATE -Wall -Wextra)

add_executable(ta_demo src/main.cpp)
target_link_libraries(ta_demo PRIVATE ta_core)
target_compile_options(ta_demo PRIVATE -Wall -Wextra)

add_executable(ta_test test/ta_test.cpp)
target_link_libraries(ta_test PRIVATE ta_core)
target_compile_options(ta_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (ta/constraints.hpp): the same provider decomposed into
# one component each on the engine's DecomposedScoreCalculator, checked
# bit-for-bit against the monolithic calculators.
add_executable(ta_plugin_test test/ta_plugin_test.cpp)
target_link_libraries(ta_plugin_test PRIVATE ta_core)
target_compile_options(ta_plugin_test PRIVATE -Wall -Wextra)
