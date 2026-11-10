# task_assigning: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(ta_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(ta_core PUBLIC include)
target_link_libraries(ta_core PUBLIC optimaru)
target_compile_options(ta_core PRIVATE -Wall -Wextra)

add_executable(ta_demo src/main.cpp)
target_link_libraries(ta_demo PRIVATE ta_core)
target_compile_options(ta_demo PRIVATE -Wall -Wextra)

add_executable(ta_test ${OPTIMARU_TESTS_DIR}/task_assigning/cpp/ta_test.cpp)
target_link_libraries(ta_test PRIVATE ta_core)
target_compile_options(ta_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (ta/constraints.hpp): the same provider decomposed into
# one component each on the engine's DecomposedObjective, checked
# bit-for-bit against the monolithic calculators.
add_executable(ta_plugin_test ${OPTIMARU_TESTS_DIR}/task_assigning/cpp/ta_plugin_test.cpp)
target_link_libraries(ta_plugin_test PRIVATE ta_core)
target_compile_options(ta_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME ta_test COMMAND ta_test)
add_test(NAME ta_plugin_test COMMAND ta_plugin_test)
