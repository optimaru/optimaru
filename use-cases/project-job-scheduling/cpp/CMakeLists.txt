cmake_minimum_required(VERSION 3.16)
project(project_job_scheduling_cpp CXX)

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

add_library(pjs_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(pjs_core PUBLIC include)
target_link_libraries(pjs_core PUBLIC optimaru)
target_compile_options(pjs_core PRIVATE -Wall -Wextra)

add_executable(pjs_demo src/main.cpp)
target_link_libraries(pjs_demo PRIVATE pjs_core)
target_compile_options(pjs_demo PRIVATE -Wall -Wextra)

add_executable(pjs_test test/pjs_test.cpp)
target_link_libraries(pjs_test PRIVATE pjs_core)
target_compile_options(pjs_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (pjs/constraints.hpp): the same provider decomposed
# into one component each on the engine's DecomposedScoreCalculator, checked
# bit-for-bit against the monolithic calculators.
add_executable(pjs_plugin_test test/pjs_plugin_test.cpp)
target_link_libraries(pjs_plugin_test PRIVATE pjs_core)
target_compile_options(pjs_plugin_test PRIVATE -Wall -Wextra)
