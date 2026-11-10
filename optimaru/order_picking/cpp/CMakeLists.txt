# order_picking: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(op_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
    src/demo_data.cpp
)
target_include_directories(op_core PUBLIC include)
target_link_libraries(op_core PUBLIC optimaru)
target_compile_options(op_core PRIVATE -Wall -Wextra)

add_executable(op_demo src/main.cpp)
target_link_libraries(op_demo PRIVATE op_core)
target_compile_options(op_demo PRIVATE -Wall -Wextra)

add_executable(op_test ${OPTIMARU_TESTS_DIR}/order_picking/cpp/op_test.cpp)
target_link_libraries(op_test PRIVATE op_core)
target_compile_options(op_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (op/constraints.hpp): the same provider decomposed into
# one component each on the engine's DecomposedObjective, checked
# bit-for-bit against the monolithic calculators.
add_executable(op_plugin_test ${OPTIMARU_TESTS_DIR}/order_picking/cpp/op_plugin_test.cpp)
target_link_libraries(op_plugin_test PRIVATE op_core)
target_compile_options(op_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME op_test COMMAND op_test)
add_test(NAME op_plugin_test COMMAND op_plugin_test)
