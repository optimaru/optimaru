# vehicle_routing: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(vrp_core
    vrp/model/haversine.cpp
    vrp/score/score_calculator.cpp
    vrp/score/incremental_calculator.cpp
    vrp/solver.cpp
    vrp/model/demo_data.cpp
    vrp/moves/visit_insertion.cpp
    vrp/moves/group_moves.cpp
)
target_include_directories(vrp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vrp_core PUBLIC optimaru)
target_compile_options(vrp_core PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
target_link_libraries(vrp_core PUBLIC Threads::Threads)

add_executable(vrp_demo main.cpp)
target_link_libraries(vrp_demo PRIVATE vrp_core)
target_compile_options(vrp_demo PRIVATE -Wall -Wextra)

add_executable(vrp_test ${OPTIMARU_TESTS_DIR}/vehicle_routing/cpp/vrp_test.cpp)
target_link_libraries(vrp_test PRIVATE vrp_core)
target_compile_options(vrp_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (vrp/constraints.hpp): the same provider decomposed
# into one component each on the engine's DecomposedObjective, checked
# bit-for-bit against ScoreCalculator::score.
add_executable(vrp_plugin_test ${OPTIMARU_TESTS_DIR}/vehicle_routing/cpp/vrp_plugin_test.cpp)
target_link_libraries(vrp_plugin_test PRIVATE vrp_core)
target_compile_options(vrp_plugin_test PRIVATE -Wall -Wextra)

# Visit groups: the insertion scan, the order of a group's visits and the group
# operator (vrp/moves/visit_insertion.hpp, vrp/moves/group_moves.hpp).
add_executable(vrp_group_moves_test ${OPTIMARU_TESTS_DIR}/vehicle_routing/cpp/vrp_group_moves_test.cpp)
target_link_libraries(vrp_group_moves_test PRIVATE vrp_core)
target_compile_options(vrp_group_moves_test PRIVATE -Wall -Wextra)

add_test(NAME vrp_test COMMAND vrp_test)
add_test(NAME vrp_plugin_test COMMAND vrp_plugin_test)
add_test(NAME vrp_group_moves_test COMMAND vrp_group_moves_test)
