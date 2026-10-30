cmake_minimum_required(VERSION 3.16)
project(employee_scheduling_cpp CXX)

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

add_library(es_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(es_core PUBLIC include)
target_link_libraries(es_core PUBLIC optimaru)
target_compile_options(es_core PRIVATE -Wall -Wextra)

# Checks every default constraint in isolation, runs the incremental calculator
# against a full recompute over a long random walk, then solves a small roster.
add_executable(es_demo src/main.cpp)
target_link_libraries(es_demo PRIVATE es_core)
target_compile_options(es_demo PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME es_demo COMMAND es_demo 5 4 1)
