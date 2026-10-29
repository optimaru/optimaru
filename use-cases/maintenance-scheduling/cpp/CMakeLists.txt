cmake_minimum_required(VERSION 3.16)
project(maintenance_scheduling_cpp CXX)

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

add_library(mnt_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
    src/demo_data.cpp
)
target_include_directories(mnt_core PUBLIC include)
target_link_libraries(mnt_core PUBLIC optimaru)
target_compile_options(mnt_core PRIVATE -Wall -Wextra)

add_executable(mnt_demo src/main.cpp)
target_link_libraries(mnt_demo PRIVATE mnt_core)
target_compile_options(mnt_demo PRIVATE -Wall -Wextra)

add_executable(mnt_test test/mnt_test.cpp)
target_link_libraries(mnt_test PRIVATE mnt_core)
target_compile_options(mnt_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (mnt/constraints.hpp): the same provider decomposed
# into one component each on the engine's DecomposedScoreCalculator, checked
# bit-for-bit against the monolithic calculators.
add_executable(mnt_plugin_test test/mnt_plugin_test.cpp)
target_link_libraries(mnt_plugin_test PRIVATE mnt_core)
target_compile_options(mnt_plugin_test PRIVATE -Wall -Wextra)
