# box_sizing: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

find_package(Threads REQUIRED)

add_library(bs_core
    bs/score/score_calculator.cpp
    bs/score/incremental_calculator.cpp
    bs/construct/construction.cpp
    bs/solver.cpp
)
target_include_directories(bs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bs_core PUBLIC optimaru Threads::Threads)
target_compile_options(bs_core PRIVATE -Wall -Wextra)

add_executable(bs_demo main.cpp)
target_link_libraries(bs_demo PRIVATE bs_core)
target_compile_options(bs_demo PRIVATE -Wall -Wextra)

add_executable(bs_test ${OPTIMARU_TESTS_DIR}/box_sizing/cpp/bs_test.cpp)
target_link_libraries(bs_test PRIVATE bs_core)
target_compile_options(bs_test PRIVATE -Wall -Wextra)

# Plugin-assembly path (bs/constraints.hpp): the same score decomposed into
# one component per constraint on the engine's DecomposedObjective, checked
# against the monolithic calculators.
add_executable(bs_plugin_test ${OPTIMARU_TESTS_DIR}/box_sizing/cpp/bs_plugin_test.cpp)
target_link_libraries(bs_plugin_test PRIVATE bs_core)
target_compile_options(bs_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME bs_test COMMAND bs_test)
add_test(NAME bs_plugin_test COMMAND bs_plugin_test)
