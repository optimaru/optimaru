cmake_minimum_required(VERSION 3.16)
project(flight_crew_scheduling_cpp CXX)

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

add_library(fcs_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(fcs_core PUBLIC include)
target_link_libraries(fcs_core PUBLIC optimaru)
target_compile_options(fcs_core PRIVATE -Wall -Wextra)

add_executable(fcs_demo src/main.cpp)
target_link_libraries(fcs_demo PRIVATE fcs_core)
target_compile_options(fcs_demo PRIVATE -Wall -Wextra)

add_executable(fcs_test test/fcs_test.cpp)
target_link_libraries(fcs_test PRIVATE fcs_core)
target_compile_options(fcs_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (fcs/constraints.hpp): the same provider decomposed
# into one component each on the engine's DecomposedScoreCalculator, checked
# bit-for-bit against the monolithic calculators.
add_executable(fcs_plugin_test test/fcs_plugin_test.cpp)
target_link_libraries(fcs_plugin_test PRIVATE fcs_core)
target_compile_options(fcs_plugin_test PRIVATE -Wall -Wextra)
