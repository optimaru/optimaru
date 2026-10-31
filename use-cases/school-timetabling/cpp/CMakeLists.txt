cmake_minimum_required(VERSION 3.16)
project(school_timetabling_cpp CXX)

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

add_executable(st_test test/st_test.cpp)
target_link_libraries(st_test PRIVATE st_core)
target_compile_options(st_test PRIVATE -Wall -Wextra)

# Canonical full-parity domain (st::canon): the CP-SAT package's H1~H10 /
# S1~S6 on the same engine. Kept as a separate library so the quickstart
# domain above stays byte-identical for the Java cross-checks.
add_library(st_canon
    src/canonical_score_calculator.cpp
    src/canonical_incremental_calculator.cpp
    src/canonical_solver.cpp
)
target_include_directories(st_canon PUBLIC include)
target_link_libraries(st_canon PUBLIC optimaru)
target_compile_options(st_canon PRIVATE -Wall -Wextra)

add_executable(canonical_test test/canonical_test.cpp)
target_link_libraries(canonical_test PRIVATE st_canon)
target_compile_options(canonical_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (st/canonical_constraints.hpp): the same H1~H10 /
# S1~S6 decomposed into one component each on the engine's
# DecomposedScoreCalculator, checked bit-for-bit against the monolithic
# calculators.
add_executable(canonical_plugin_test test/canonical_plugin_test.cpp)
target_link_libraries(canonical_plugin_test PRIVATE st_canon)
target_compile_options(canonical_plugin_test PRIVATE -Wall -Wextra)

# Solution-quality benchmark vs CP-SAT (see
# bench/st_quality_bench.cpp and benchmarks/school-timetabling/run_bench.py).
add_executable(st_quality_bench bench/st_quality_bench.cpp)
target_link_libraries(st_quality_bench PRIVATE st_core)
target_compile_options(st_quality_bench PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME st_test COMMAND st_test)
add_test(NAME canonical_test COMMAND canonical_test)
add_test(NAME canonical_plugin_test COMMAND canonical_plugin_test)
