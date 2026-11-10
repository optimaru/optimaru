# facility_location: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(fl_core
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
)
target_include_directories(fl_core PUBLIC include)
target_link_libraries(fl_core PUBLIC optimaru)
target_compile_options(fl_core PRIVATE -Wall -Wextra)

add_executable(fl_demo src/main.cpp)
target_link_libraries(fl_demo PRIVATE fl_core)
target_compile_options(fl_demo PRIVATE -Wall -Wextra)

add_executable(fl_test ${OPTIMARU_TESTS_DIR}/facility_location/cpp/fl_test.cpp)
target_link_libraries(fl_test PRIVATE fl_core)
target_compile_options(fl_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (fl/constraints.hpp): the same provider decomposed into
# one component each on the engine's DecomposedObjective, checked
# bit-for-bit against the monolithic calculators.
add_executable(fl_plugin_test ${OPTIMARU_TESTS_DIR}/facility_location/cpp/fl_plugin_test.cpp)
target_link_libraries(fl_plugin_test PRIVATE fl_core)
target_compile_options(fl_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME fl_test COMMAND fl_test)
add_test(NAME fl_plugin_test COMMAND fl_plugin_test)
