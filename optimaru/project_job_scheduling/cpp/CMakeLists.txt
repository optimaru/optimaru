# project_job_scheduling: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(pjs_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(pjs_core PUBLIC include)
target_link_libraries(pjs_core PUBLIC optimaru)
target_compile_options(pjs_core PRIVATE -Wall -Wextra)

add_executable(pjs_demo src/main.cpp)
target_link_libraries(pjs_demo PRIVATE pjs_core)
target_compile_options(pjs_demo PRIVATE -Wall -Wextra)

add_executable(pjs_test ${OPTIMARU_TESTS_DIR}/project_job_scheduling/cpp/pjs_test.cpp)
target_link_libraries(pjs_test PRIVATE pjs_core)
target_compile_options(pjs_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (pjs/constraints.hpp): the same provider decomposed
# into one component each on the engine's DecomposedObjective, checked
# bit-for-bit against the monolithic calculators.
add_executable(pjs_plugin_test ${OPTIMARU_TESTS_DIR}/project_job_scheduling/cpp/pjs_plugin_test.cpp)
target_link_libraries(pjs_plugin_test PRIVATE pjs_core)
target_compile_options(pjs_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME pjs_test COMMAND pjs_test)
add_test(NAME pjs_plugin_test COMMAND pjs_plugin_test)
