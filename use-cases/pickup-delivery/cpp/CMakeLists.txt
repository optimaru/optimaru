cmake_minimum_required(VERSION 3.16)
project(pickup_delivery_cpp CXX)

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

add_library(pdp_core
    src/haversine.cpp
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
    src/demo_data.cpp
)
target_include_directories(pdp_core PUBLIC include)
target_link_libraries(pdp_core PUBLIC optimaru)
target_compile_options(pdp_core PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
target_link_libraries(pdp_core PUBLIC Threads::Threads)

add_executable(pdp_demo src/main.cpp)
target_link_libraries(pdp_demo PRIVATE pdp_core)
target_compile_options(pdp_demo PRIVATE -Wall -Wextra)

add_executable(pdp_test test/pdp_test.cpp)
target_link_libraries(pdp_test PRIVATE pdp_core)
target_compile_options(pdp_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (pdp/constraints.hpp): the same constraint set
# decomposed into one component each on the engine's
# DecomposedScoreCalculator, checked bit-for-bit against
# ScoreCalculator::calculateScore.
add_executable(pdp_plugin_test test/pdp_plugin_test.cpp)
target_link_libraries(pdp_plugin_test PRIVATE pdp_core)
target_compile_options(pdp_plugin_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME pdp_test COMMAND pdp_test)
add_test(NAME pdp_plugin_test COMMAND pdp_plugin_test)
