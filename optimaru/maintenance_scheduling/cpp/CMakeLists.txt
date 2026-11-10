# maintenance_scheduling: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(mnt_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
    src/demo_data.cpp
)
target_include_directories(mnt_core PUBLIC include)
target_link_libraries(mnt_core PUBLIC optimaru)
target_compile_options(mnt_core PRIVATE -Wall -Wextra)

add_executable(mnt_demo src/main.cpp)
target_link_libraries(mnt_demo PRIVATE mnt_core)
target_compile_options(mnt_demo PRIVATE -Wall -Wextra)

add_executable(mnt_test ${OPTIMARU_TESTS_DIR}/maintenance_scheduling/cpp/mnt_test.cpp)
target_link_libraries(mnt_test PRIVATE mnt_core)
target_compile_options(mnt_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (mnt/constraints.hpp): the same provider decomposed
# into one component each on the engine's DecomposedObjective, checked
# bit-for-bit against the monolithic calculators.
add_executable(mnt_plugin_test ${OPTIMARU_TESTS_DIR}/maintenance_scheduling/cpp/mnt_plugin_test.cpp)
target_link_libraries(mnt_plugin_test PRIVATE mnt_core)
target_compile_options(mnt_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME mnt_test COMMAND mnt_test)
add_test(NAME mnt_plugin_test COMMAND mnt_plugin_test)
