# tournament_scheduling: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(ts_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(ts_core PUBLIC include)
target_link_libraries(ts_core PUBLIC optimaru)
target_compile_options(ts_core PRIVATE -Wall -Wextra)

add_executable(ts_demo src/main.cpp)
target_link_libraries(ts_demo PRIVATE ts_core)
target_compile_options(ts_demo PRIVATE -Wall -Wextra)

add_executable(ts_test ${OPTIMARU_TESTS_DIR}/tournament_scheduling/cpp/ts_test.cpp)
target_link_libraries(ts_test PRIVATE ts_core)
target_compile_options(ts_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (ts/constraints.hpp): the same constraints decomposed
# into one component each on the engine's DecomposedObjective, checked
# bit-for-bit against the monolithic calculators.
add_executable(ts_plugin_test ${OPTIMARU_TESTS_DIR}/tournament_scheduling/cpp/ts_plugin_test.cpp)
target_link_libraries(ts_plugin_test PRIVATE ts_core)
target_compile_options(ts_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME ts_test COMMAND ts_test)
add_test(NAME ts_plugin_test COMMAND ts_plugin_test)
