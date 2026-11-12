# meeting_scheduling: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(ms_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(ms_core PUBLIC include)
target_link_libraries(ms_core PUBLIC optimaru)
target_compile_options(ms_core PRIVATE -Wall -Wextra)

add_executable(ms_demo src/main.cpp)
target_link_libraries(ms_demo PRIVATE ms_core)
target_compile_options(ms_demo PRIVATE -Wall -Wextra)

add_executable(ms_test ${OPTIMARU_TESTS_DIR}/meeting_scheduling/cpp/ms_test.cpp)
target_link_libraries(ms_test PRIVATE ms_core)
target_compile_options(ms_test PRIVATE -Wall -Wextra)

# Quality-benchmark driver over the dumped comparison instances
# (benchmarks/meeting-scheduling/dump_instances.py), with knob overrides for A/B sweeps.
# Not part of the demo or the test gate.
add_executable(ms_quality_bench ${OPTIMARU_BENCHMARKS_DIR}/meeting-scheduling/cpp/ms_quality_bench.cpp)
target_link_libraries(ms_quality_bench PRIVATE ms_core)
target_compile_options(ms_quality_bench PRIVATE -Wall -Wextra)

# Plugin-assembly path (ms/constraints.hpp): the same provider decomposed into
# one component each on the engine's DecomposedObjective, checked
# bit-for-bit against the monolithic calculators.
add_executable(ms_plugin_test ${OPTIMARU_TESTS_DIR}/meeting_scheduling/cpp/ms_plugin_test.cpp)
target_link_libraries(ms_plugin_test PRIVATE ms_core)
target_compile_options(ms_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME ms_test COMMAND ms_test)
add_test(NAME ms_plugin_test COMMAND ms_plugin_test)
