# assembly_line_balancing: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(alb_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/construction.cpp
    src/solver.cpp
)
target_include_directories(alb_core PUBLIC include)
target_link_libraries(alb_core PUBLIC optimaru)
target_compile_options(alb_core PRIVATE -Wall -Wextra)

add_executable(alb_demo src/main.cpp)
target_link_libraries(alb_demo PRIVATE alb_core)
target_compile_options(alb_demo PRIVATE -Wall -Wextra)

# Monolithic path: per-constraint hand checks, the ported construction,
# incremental == full recompute on random moves, ScoreChecks::Full solves.
add_executable(alb_test ${OPTIMARU_TESTS_DIR}/assembly_line_balancing/cpp/alb_test.cpp)
target_link_libraries(alb_test PRIVATE alb_core)
target_compile_options(alb_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (alb/constraints.hpp): the same score decomposed into
# one component per constraint on the engine's DecomposedObjective,
# checked bit-for-bit against the monolithic calculators.
add_executable(alb_plugin_test ${OPTIMARU_TESTS_DIR}/assembly_line_balancing/cpp/alb_plugin_test.cpp)
target_link_libraries(alb_plugin_test PRIVATE alb_core)
target_compile_options(alb_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME alb_test COMMAND alb_test)
add_test(NAME alb_plugin_test COMMAND alb_plugin_test)
