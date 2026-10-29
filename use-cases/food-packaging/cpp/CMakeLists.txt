cmake_minimum_required(VERSION 3.16)
project(food_packaging_cpp CXX)

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

add_library(fp_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
    src/demo_data.cpp
)
target_include_directories(fp_core PUBLIC include)
target_link_libraries(fp_core PUBLIC optimaru)
target_compile_options(fp_core PRIVATE -Wall -Wextra)

add_executable(fp_demo src/main.cpp)
target_link_libraries(fp_demo PRIVATE fp_core)
target_compile_options(fp_demo PRIVATE -Wall -Wextra)

add_executable(fp_test test/fp_test.cpp)
target_link_libraries(fp_test PRIVATE fp_core)
target_compile_options(fp_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (fp/constraints.hpp): the same provider decomposed into
# one component each on the engine's DecomposedScoreCalculator, checked
# bit-for-bit against the monolithic calculators.
add_executable(fp_plugin_test test/fp_plugin_test.cpp)
target_link_libraries(fp_plugin_test PRIVATE fp_core)
target_compile_options(fp_plugin_test PRIVATE -Wall -Wextra)
