# food_packaging: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(fp_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
    src/demo_data.cpp
)
target_include_directories(fp_core PUBLIC include)
target_link_libraries(fp_core PUBLIC optimaru)
target_compile_options(fp_core PRIVATE -Wall -Wextra)

add_executable(fp_demo src/main.cpp)
target_link_libraries(fp_demo PRIVATE fp_core)
target_compile_options(fp_demo PRIVATE -Wall -Wextra)

add_executable(fp_test ${OPTIMARU_TESTS_DIR}/food_packaging/cpp/fp_test.cpp)
target_link_libraries(fp_test PRIVATE fp_core)
target_compile_options(fp_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (fp/constraints.hpp): the same constraints decomposed into
# one component each on the engine's DecomposedObjective, checked
# bit-for-bit against the monolithic calculators.
add_executable(fp_plugin_test ${OPTIMARU_TESTS_DIR}/food_packaging/cpp/fp_plugin_test.cpp)
target_link_libraries(fp_plugin_test PRIVATE fp_core)
target_compile_options(fp_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME fp_test COMMAND fp_test)
add_test(NAME fp_plugin_test COMMAND fp_plugin_test)
