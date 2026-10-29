cmake_minimum_required(VERSION 3.16)
project(facility_location_cpp CXX)

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

add_library(fl_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(fl_core PUBLIC include)
target_link_libraries(fl_core PUBLIC optimaru)
target_compile_options(fl_core PRIVATE -Wall -Wextra)

add_executable(fl_demo src/main.cpp)
target_link_libraries(fl_demo PRIVATE fl_core)
target_compile_options(fl_demo PRIVATE -Wall -Wextra)

add_executable(fl_test test/fl_test.cpp)
target_link_libraries(fl_test PRIVATE fl_core)
target_compile_options(fl_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (fl/constraints.hpp): the same provider decomposed into
# one component each on the engine's DecomposedScoreCalculator, checked
# bit-for-bit against the monolithic calculators.
add_executable(fl_plugin_test test/fl_plugin_test.cpp)
target_link_libraries(fl_plugin_test PRIVATE fl_core)
target_compile_options(fl_plugin_test PRIVATE -Wall -Wextra)
