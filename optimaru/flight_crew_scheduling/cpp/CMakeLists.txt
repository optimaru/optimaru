# flight_crew_scheduling: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(fcs_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(fcs_core PUBLIC include)
target_link_libraries(fcs_core PUBLIC optimaru)
target_compile_options(fcs_core PRIVATE -Wall -Wextra)

add_executable(fcs_demo src/main.cpp)
target_link_libraries(fcs_demo PRIVATE fcs_core)
target_compile_options(fcs_demo PRIVATE -Wall -Wextra)

add_executable(fcs_test ${OPTIMARU_TESTS_DIR}/flight_crew_scheduling/cpp/fcs_test.cpp)
target_link_libraries(fcs_test PRIVATE fcs_core)
target_compile_options(fcs_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (fcs/constraints.hpp): the same constraints decomposed
# into one component each on the engine's DecomposedObjective, checked
# bit-for-bit against the monolithic calculators.
add_executable(fcs_plugin_test ${OPTIMARU_TESTS_DIR}/flight_crew_scheduling/cpp/fcs_plugin_test.cpp)
target_link_libraries(fcs_plugin_test PRIVATE fcs_core)
target_compile_options(fcs_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME fcs_test COMMAND fcs_test)
add_test(NAME fcs_plugin_test COMMAND fcs_plugin_test)
