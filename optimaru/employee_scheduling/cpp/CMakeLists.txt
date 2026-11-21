# employee_scheduling: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(es_core
    es/score/score_calculator.cpp
    es/score/incremental_calculator.cpp
    es/solver.cpp
)
target_include_directories(es_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(es_core PUBLIC optimaru)
target_compile_options(es_core PRIVATE -Wall -Wextra)

# Checks every default constraint in isolation, runs the incremental calculator
# against a full recompute over a long random walk, then solves a small roster.
add_executable(es_demo main.cpp)
target_link_libraries(es_demo PRIVATE es_core)
target_compile_options(es_demo PRIVATE -Wall -Wextra)

# Plugin-assembly path (es/constraints.hpp): the catalog, the default assembly
# against the monolithic calculator, incremental == full for every component,
# ScopedUnit metadata and the instance labels.
add_executable(es_plugin_test ${OPTIMARU_TESTS_DIR}/employee_scheduling/cpp/es_plugin_test.cpp)
target_link_libraries(es_plugin_test PRIVATE es_core)
target_compile_options(es_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME es_demo COMMAND es_demo 5 4 1)
add_test(NAME es_plugin_test COMMAND es_plugin_test)
