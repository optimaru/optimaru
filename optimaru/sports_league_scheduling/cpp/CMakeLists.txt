# sports_league_scheduling: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(sls_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(sls_core PUBLIC include)
target_link_libraries(sls_core PUBLIC optimaru)
target_compile_options(sls_core PRIVATE -Wall -Wextra)

add_executable(sls_demo src/main.cpp)
target_link_libraries(sls_demo PRIVATE sls_core)
target_compile_options(sls_demo PRIVATE -Wall -Wextra)

add_executable(sls_test ${OPTIMARU_TESTS_DIR}/sports_league_scheduling/cpp/sls_test.cpp)
target_link_libraries(sls_test PRIVATE sls_core)
target_compile_options(sls_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (sls/constraints.hpp): the same constraints decomposed
# into one component each on the engine's DecomposedObjective, checked
# bit-for-bit against the monolithic calculators.
add_executable(sls_plugin_test ${OPTIMARU_TESTS_DIR}/sports_league_scheduling/cpp/sls_plugin_test.cpp)
target_link_libraries(sls_plugin_test PRIVATE sls_core)
target_compile_options(sls_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME sls_test COMMAND sls_test)
add_test(NAME sls_plugin_test COMMAND sls_plugin_test)
