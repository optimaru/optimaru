# conference_scheduling: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(cs_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(cs_core PUBLIC include)
target_link_libraries(cs_core PUBLIC optimaru)
target_compile_options(cs_core PRIVATE -Wall -Wextra)

add_executable(cs_demo src/main.cpp)
target_link_libraries(cs_demo PRIVATE cs_core)
target_compile_options(cs_demo PRIVATE -Wall -Wextra)

add_executable(cs_test ${OPTIMARU_TESTS_DIR}/conference_scheduling/cpp/cs_test.cpp)
target_link_libraries(cs_test PRIVATE cs_core)
target_compile_options(cs_test PRIVATE -Wall -Wextra)

# Solution-quality benchmark (benchmarks/conference-scheduling/cpp/cs_quality_bench.cpp): demo + scaled
# instances, best-score trajectory, per-constraint breakdown, moves/s.
add_executable(cs_quality_bench ${OPTIMARU_BENCHMARKS_DIR}/conference-scheduling/cpp/cs_quality_bench.cpp)
target_link_libraries(cs_quality_bench PRIVATE cs_core)
target_compile_options(cs_quality_bench PRIVATE -Wall -Wextra)

# Plugin-assembly path (cs/constraints.hpp): the same 37 constraints,
# projected into one component each on the
# engine's DecomposedObjective, checked against the monolithic
# calculators.
add_executable(cs_plugin_test ${OPTIMARU_TESTS_DIR}/conference_scheduling/cpp/cs_plugin_test.cpp)
target_link_libraries(cs_plugin_test PRIVATE cs_core)
target_compile_options(cs_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME cs_test COMMAND cs_test)
add_test(NAME cs_plugin_test COMMAND cs_plugin_test)
