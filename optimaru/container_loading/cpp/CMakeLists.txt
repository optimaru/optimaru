# container_loading: the C++ core, its demo and its tests. Added by the root CMakeLists.txt,
# which defines the header-only engine target `optimaru`.

add_library(cl_core
    cl/score/packing.cpp
    cl/score/score_calculator.cpp
    cl/construct/construction.cpp
    cl/solver.cpp
    cl/model/demo_data.cpp
)
target_include_directories(cl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cl_core PUBLIC optimaru)
target_compile_options(cl_core PRIVATE -Wall -Wextra)

add_executable(cl_demo main.cpp)
target_link_libraries(cl_demo PRIVATE cl_core)
target_compile_options(cl_demo PRIVATE -Wall -Wextra)

# The decoder, the costs on hand-built loads, the construction, the moves and the solver.
add_executable(cl_test ${OPTIMARU_TESTS_DIR}/container_loading/cpp/cl_test.cpp)
target_link_libraries(cl_test PRIVATE cl_core)
target_compile_options(cl_test PRIVATE -Wall -Wextra)

# The assembly (cl/constraints.hpp) on the engine's DecomposedObjective, checked against the
# full recompute of ScoreCalculator, incrementally and through the move previews.
add_executable(cl_plugin_test ${OPTIMARU_TESTS_DIR}/container_loading/cpp/cl_plugin_test.cpp)
target_link_libraries(cl_plugin_test PRIVATE cl_core)
target_compile_options(cl_plugin_test PRIVATE -Wall -Wextra)

add_test(NAME cl_test COMMAND cl_test)
add_test(NAME cl_plugin_test COMMAND cl_plugin_test)
