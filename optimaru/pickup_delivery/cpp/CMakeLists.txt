# pickup_delivery: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(pdp_core
    src/haversine.cpp
    src/score_calculator.cpp
    src/incremental_calculator.cpp
    src/solver.cpp
    src/demo_data.cpp
)
target_include_directories(pdp_core PUBLIC include)
target_link_libraries(pdp_core PUBLIC optimaru)
target_compile_options(pdp_core PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
target_link_libraries(pdp_core PUBLIC Threads::Threads)

add_executable(pdp_demo src/main.cpp)
target_link_libraries(pdp_demo PRIVATE pdp_core)
target_compile_options(pdp_demo PRIVATE -Wall -Wextra)

add_executable(pdp_test ${OPTIMARU_TESTS_DIR}/pickup_delivery/cpp/pdp_test.cpp)
target_link_libraries(pdp_test PRIVATE pdp_core)
target_compile_options(pdp_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (pdp/constraints.hpp): the same constraint set
# decomposed into one component each on the engine's
# DecomposedObjective, checked bit-for-bit against
# ScoreCalculator::score.
add_executable(pdp_plugin_test ${OPTIMARU_TESTS_DIR}/pickup_delivery/cpp/pdp_plugin_test.cpp)
target_link_libraries(pdp_plugin_test PRIVATE pdp_core)
target_compile_options(pdp_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME pdp_test COMMAND pdp_test)
add_test(NAME pdp_plugin_test COMMAND pdp_plugin_test)
