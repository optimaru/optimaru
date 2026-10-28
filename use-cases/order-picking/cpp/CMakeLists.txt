cmake_minimum_required(VERSION 3.16)
project(order_picking_cpp CXX)

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

add_library(op_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
    src/demo_data.cpp
)
target_include_directories(op_core PUBLIC include)
target_link_libraries(op_core PUBLIC optimaru)
target_compile_options(op_core PRIVATE -Wall -Wextra)

add_executable(op_demo src/main.cpp)
target_link_libraries(op_demo PRIVATE op_core)
target_compile_options(op_demo PRIVATE -Wall -Wextra)

add_executable(op_test test/op_test.cpp)
target_link_libraries(op_test PRIVATE op_core)
target_compile_options(op_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (op/constraints.hpp): the same provider decomposed into
# one component each on the engine's DecomposedScoreCalculator, checked
# bit-for-bit against the monolithic calculators.
add_executable(op_plugin_test test/op_plugin_test.cpp)
target_link_libraries(op_plugin_test PRIVATE op_core)
target_compile_options(op_plugin_test PRIVATE -Wall -Wextra)
