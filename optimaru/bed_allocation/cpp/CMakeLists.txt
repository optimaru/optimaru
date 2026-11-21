# bed_allocation: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(ba_core
    ba/score/score_calculator.cpp
    ba/score/incremental_calculator.cpp
    ba/solver.cpp
)
target_include_directories(ba_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ba_core PUBLIC optimaru)
target_compile_options(ba_core PRIVATE -Wall -Wextra)

add_executable(ba_demo main.cpp)
target_link_libraries(ba_demo PRIVATE ba_core)
target_compile_options(ba_demo PRIVATE -Wall -Wextra)

add_executable(ba_test ${OPTIMARU_TESTS_DIR}/bed_allocation/cpp/ba_test.cpp)
target_link_libraries(ba_test PRIVATE ba_core)
target_compile_options(ba_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (ba/constraints.hpp): the same constraints decomposed into
# one component each on the engine's DecomposedObjective, checked
# bit-for-bit against the monolithic calculators.
add_executable(ba_plugin_test ${OPTIMARU_TESTS_DIR}/bed_allocation/cpp/ba_plugin_test.cpp)
target_link_libraries(ba_plugin_test PRIVATE ba_core)
target_compile_options(ba_plugin_test PRIVATE -Wall -Wextra)

# Engine-only quality bench over the dumped comparison instances
# (benchmarks/bed-allocation/dump_instances.py); used for interleaved A/B tuning runs.
add_executable(ba_quality_bench ${OPTIMARU_BENCHMARKS_DIR}/bed-allocation/cpp/ba_quality_bench.cpp)
target_link_libraries(ba_quality_bench PRIVATE ba_core)
target_compile_options(ba_quality_bench PRIVATE -Wall -Wextra)

add_test(NAME ba_test COMMAND ba_test)
add_test(NAME ba_plugin_test COMMAND ba_plugin_test)
