# school_timetabling: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(st_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(st_core PUBLIC include)
target_link_libraries(st_core PUBLIC optimaru)
target_compile_options(st_core PRIVATE -Wall -Wextra)

add_executable(st_demo src/main.cpp)
target_link_libraries(st_demo PRIVATE st_core)
target_compile_options(st_demo PRIVATE -Wall -Wextra)

add_executable(st_test ${OPTIMARU_TESTS_DIR}/school_timetabling/cpp/st_test.cpp)
target_link_libraries(st_test PRIVATE st_core)
target_compile_options(st_test PRIVATE -Wall -Wextra)

# Canonical full-parity domain (st::canon): the CP-SAT package's hard and
# soft constraints on the same engine. Kept as a separate library so the quickstart
# domain above stays byte-identical for the evaluate.py cross-check.
add_library(st_canon
    src/canonical_score_calculator.cpp
    src/canonical_incremental_calculator.cpp
    src/canonical_solver.cpp
)
target_include_directories(st_canon PUBLIC include)
target_link_libraries(st_canon PUBLIC optimaru)
target_compile_options(st_canon PRIVATE -Wall -Wextra)

add_executable(st_canonical_test ${OPTIMARU_TESTS_DIR}/school_timetabling/cpp/st_canonical_test.cpp)
target_link_libraries(st_canonical_test PRIVATE st_canon)
target_compile_options(st_canonical_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (st/canonical_constraints.hpp): the same hard and
# soft constraints decomposed into one component each on the engine's
# DecomposedObjective, checked bit-for-bit against the monolithic
# calculators.
add_executable(st_canonical_plugin_test ${OPTIMARU_TESTS_DIR}/school_timetabling/cpp/st_canonical_plugin_test.cpp)
target_link_libraries(st_canonical_plugin_test PRIVATE st_canon)
target_compile_options(st_canonical_plugin_test PRIVATE -Wall -Wextra)

# Solution-quality benchmark vs CP-SAT (benchmarks/school-timetabling/cpp/st_quality_bench.cpp,
# driven by benchmarks/school-timetabling/run_bench.py).
add_executable(st_quality_bench ${OPTIMARU_BENCHMARKS_DIR}/school-timetabling/cpp/st_quality_bench.cpp)
target_link_libraries(st_quality_bench PRIVATE st_core)
target_compile_options(st_quality_bench PRIVATE -Wall -Wextra)

add_test(NAME st_test COMMAND st_test)
add_test(NAME st_canonical_test COMMAND st_canonical_test)
add_test(NAME st_canonical_plugin_test COMMAND st_canonical_plugin_test)
