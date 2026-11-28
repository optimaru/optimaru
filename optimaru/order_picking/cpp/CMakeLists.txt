# order_picking: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(op_core
    op/score/score_calculator.cpp
    op/score/incremental_calculator.cpp
    op/solver.cpp
    op/model/demo_data.cpp
    op/model/layout.cpp
)
target_include_directories(op_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(op_core PUBLIC optimaru)
target_compile_options(op_core PRIVATE -Wall -Wextra)

add_executable(op_demo main.cpp)
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
