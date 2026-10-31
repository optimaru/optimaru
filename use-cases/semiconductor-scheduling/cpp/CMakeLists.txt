cmake_minimum_required(VERSION 3.16)
project(semiconductor_scheduling_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The generic engine (header-only), consumed as an external header-only
# dependency.
add_library(optimaru INTERFACE)
target_include_directories(optimaru
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)

find_package(Threads REQUIRED)

add_library(fab_core
    src/decoder.cpp
    src/score_calculator.cpp
    src/evaluator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(fab_core PUBLIC include)
target_link_libraries(fab_core PUBLIC optimaru Threads::Threads)
target_compile_options(fab_core PRIVATE -Wall -Wextra)

add_executable(fab_demo src/main.cpp)
target_link_libraries(fab_demo PRIVATE fab_core)
target_compile_options(fab_demo PRIVATE -Wall -Wextra)

# Decoder fixtures, hand-calculated positive and zero cases per constraint,
# incremental == full on random move sequences, solver contracts.
add_executable(fab_test test/fab_test.cpp)
target_link_libraries(fab_test PRIVATE fab_core)
target_compile_options(fab_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (fab/constraints.hpp): the same terms as one component
# each on the engine's DecomposedScoreCalculator, checked bit-for-bit against
# the monolithic calculators.
add_executable(fab_plugin_test test/fab_plugin_test.cpp)
target_link_libraries(fab_plugin_test PRIVATE fab_core)
target_compile_options(fab_plugin_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME fab_test COMMAND fab_test)
add_test(NAME fab_plugin_test COMMAND fab_plugin_test)
