# bin_packing: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

find_package(Threads REQUIRED)

add_library(bp_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/construction.cpp
    src/solver.cpp
)
target_include_directories(bp_core PUBLIC include)
target_link_libraries(bp_core PUBLIC optimaru Threads::Threads)
target_compile_options(bp_core PRIVATE -Wall -Wextra)

add_executable(bp_demo src/main.cpp)
target_link_libraries(bp_demo PRIVATE bp_core)
target_compile_options(bp_demo PRIVATE -Wall -Wextra)

add_executable(bp_test ${OPTIMARU_TESTS_DIR}/bin_packing/cpp/bp_test.cpp)
target_link_libraries(bp_test PRIVATE bp_core)
target_compile_options(bp_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (bp/constraints.hpp): the same score decomposed into
# one component per constraint on the engine's DecomposedObjective,
# checked bit-for-bit against the monolithic calculators.
add_executable(bp_plugin_test ${OPTIMARU_TESTS_DIR}/bin_packing/cpp/bp_plugin_test.cpp)
target_link_libraries(bp_plugin_test PRIVATE bp_core)
target_compile_options(bp_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME bp_test COMMAND bp_test)
add_test(NAME bp_plugin_test COMMAND bp_plugin_test)
