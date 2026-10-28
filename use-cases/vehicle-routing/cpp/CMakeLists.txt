cmake_minimum_required(VERSION 3.16)
project(vehicle_routing_cpp CXX)

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

add_library(vrp_core
    src/haversine.cpp
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
    src/demo_data.cpp
)
target_include_directories(vrp_core PUBLIC include)
target_link_libraries(vrp_core PUBLIC optimaru)
target_compile_options(vrp_core PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
target_link_libraries(vrp_core PUBLIC Threads::Threads)

add_executable(vrp_demo src/main.cpp)
target_link_libraries(vrp_demo PRIVATE vrp_core)
target_compile_options(vrp_demo PRIVATE -Wall -Wextra)

add_executable(vrp_test test/vrp_test.cpp)
target_link_libraries(vrp_test PRIVATE vrp_core)
target_compile_options(vrp_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (vrp/constraints.hpp): the same provider decomposed
# into one component each on the engine's DecomposedScoreCalculator, checked
# bit-for-bit against ScoreCalculator::calculateScore.
add_executable(vrp_plugin_test test/vrp_plugin_test.cpp)
target_link_libraries(vrp_plugin_test PRIVATE vrp_core)
target_compile_options(vrp_plugin_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME vrp_test COMMAND vrp_test)
add_test(NAME vrp_plugin_test COMMAND vrp_plugin_test)
