cmake_minimum_required(VERSION 3.16)
project(sports_league_scheduling_cpp CXX)

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

add_library(sls_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(sls_core PUBLIC include)
target_link_libraries(sls_core PUBLIC optimaru)
target_compile_options(sls_core PRIVATE -Wall -Wextra)

add_executable(sls_demo src/main.cpp)
target_link_libraries(sls_demo PRIVATE sls_core)
target_compile_options(sls_demo PRIVATE -Wall -Wextra)

add_executable(sls_test test/sls_test.cpp)
target_link_libraries(sls_test PRIVATE sls_core)
target_compile_options(sls_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (sls/constraints.hpp): the same provider decomposed
# into one component each on the engine's DecomposedScoreCalculator, checked
# bit-for-bit against the monolithic calculators.
add_executable(sls_plugin_test test/sls_plugin_test.cpp)
target_link_libraries(sls_plugin_test PRIVATE sls_core)
target_compile_options(sls_plugin_test PRIVATE -Wall -Wextra)
