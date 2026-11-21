# quadratic_assignment: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(qap_core
    qap/score/score_calculator.cpp
    qap/score/incremental_calculator.cpp
    qap/solver.cpp
)
target_include_directories(qap_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qap_core PUBLIC optimaru)
target_compile_options(qap_core PRIVATE -Wall -Wextra)

add_executable(qap_demo main.cpp)
target_link_libraries(qap_demo PRIVATE qap_core)
target_compile_options(qap_demo PRIVATE -Wall -Wextra)

# Monolithic path: per-constraint hand checks, incremental == full recompute
# on random moves, ScoreChecks::Full, warm starts, the bijection guard and
# brute-force solves.
add_executable(qap_test ${OPTIMARU_TESTS_DIR}/quadratic_assignment/cpp/qap_test.cpp)
target_link_libraries(qap_test PRIVATE qap_core)
target_compile_options(qap_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (qap/constraints.hpp): the same score decomposed into
# one component per constraint on the engine's DecomposedObjective,
# checked bit-for-bit against the monolithic calculators.
add_executable(qap_plugin_test ${OPTIMARU_TESTS_DIR}/quadratic_assignment/cpp/qap_plugin_test.cpp)
target_link_libraries(qap_plugin_test PRIVATE qap_core)
target_compile_options(qap_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME qap_test COMMAND qap_test)
add_test(NAME qap_plugin_test COMMAND qap_plugin_test)
