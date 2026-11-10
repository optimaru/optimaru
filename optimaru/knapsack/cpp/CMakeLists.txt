# knapsack: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(ks_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/construction.cpp
    src/solver.cpp
)
target_include_directories(ks_core PUBLIC include)
target_link_libraries(ks_core PUBLIC optimaru)
target_compile_options(ks_core PRIVATE -Wall -Wextra)

add_executable(ks_demo src/main.cpp)
target_link_libraries(ks_demo PRIVATE ks_core)
target_compile_options(ks_demo PRIVATE -Wall -Wextra)

# Monolithic path: per-constraint hand checks, the greedy construction,
# incremental == full recompute on random moves, ScoreChecks::Full and brute-force
# solves.
add_executable(ks_test ${OPTIMARU_TESTS_DIR}/knapsack/cpp/ks_test.cpp)
target_link_libraries(ks_test PRIVATE ks_core)
target_compile_options(ks_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (ks/constraints.hpp): the same score decomposed into
# one component per constraint on the engine's DecomposedObjective,
# checked bit-for-bit against the monolithic calculators.
add_executable(ks_plugin_test ${OPTIMARU_TESTS_DIR}/knapsack/cpp/ks_plugin_test.cpp)
target_link_libraries(ks_plugin_test PRIVATE ks_core)
target_compile_options(ks_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME ks_test COMMAND ks_test)
add_test(NAME ks_plugin_test COMMAND ks_plugin_test)
