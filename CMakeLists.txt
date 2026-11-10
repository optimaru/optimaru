cmake_minimum_required(VERSION 3.16)
project(optimaru CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The engine: header-only, in optimaru/cpp/optimaru/ (#include "optimaru/...").
# Multi-start solves run on std::thread, so consumers link the platform's
# thread library through the target.
find_package(Threads REQUIRED)
add_library(optimaru INTERFACE)
add_library(optimaru::optimaru ALIAS optimaru)
target_include_directories(optimaru INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/optimaru/cpp)
target_compile_features(optimaru INTERFACE cxx_std_17)
target_link_libraries(optimaru INTERFACE Threads::Threads)

# Tests, demos and benches only when built standalone, not when pulled in via
# add_subdirectory.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    set(OPTIMARU_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    # Engine tests.
    option(OPTIMARU_ENGINE_TESTS "Build the engine tests in tests/engine" ON)
    if(OPTIMARU_ENGINE_TESTS)
        add_executable(engine_test tests/engine/engine_test.cpp)
        target_link_libraries(engine_test PRIVATE optimaru Threads::Threads)
        target_compile_options(engine_test PRIVATE -Wall -Wextra)

        # Levels, the medium fold and the knob guards.
        add_executable(constraint_level_test tests/engine/constraint_level_test.cpp)
        target_link_libraries(constraint_level_test PRIVATE optimaru)
        target_compile_options(constraint_level_test PRIVATE -Wall -Wextra)

        add_test(NAME engine_test COMMAND engine_test)
        add_test(NAME constraint_level_test COMMAND constraint_level_test)
    endif()

    # Use cases: each optimaru/<use case>/cpp/ builds its core library, demo,
    # benches and the C++ tests in tests/<use case>/cpp/.
    set(OPTIMARU_ALL_USE_CASES
        assembly_line_balancing bed_allocation bin_packing conference_scheduling
        employee_scheduling facility_location flight_crew_scheduling food_packaging
        knapsack maintenance_scheduling max_cut meeting_scheduling order_picking
        pickup_delivery project_job_scheduling quadratic_assignment school_timetabling
        semiconductor_scheduling sports_league_scheduling task_assigning
        tournament_scheduling vehicle_routing)
    set(OPTIMARU_USE_CASES "all" CACHE STRING
        "Use cases to build: \"all\", \"none\" or a list such as knapsack,vehicle_routing")
    if(OPTIMARU_USE_CASES STREQUAL "all")
        set(selected ${OPTIMARU_ALL_USE_CASES})
    elseif(OPTIMARU_USE_CASES STREQUAL "none")
        set(selected "")
    else()
        # The same spelling setup.py takes: commas, spaces (or CMake's ;) between names.
        string(REGEX REPLACE "[ ,;]+" ";" selected "${OPTIMARU_USE_CASES}")
        string(REGEX REPLACE "^;|;$" "" selected "${selected}")
    endif()
    foreach(use_case IN LISTS selected)
        if(NOT use_case IN_LIST OPTIMARU_ALL_USE_CASES)
            message(FATAL_ERROR "OPTIMARU_USE_CASES: unknown use case '${use_case}'")
        endif()
        add_subdirectory(optimaru/${use_case}/cpp ${use_case})
    endforeach()
endif()
