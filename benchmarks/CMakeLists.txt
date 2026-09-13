# Benchmarks.
#
# Separate from the test-suite because they report rather than assert. A
# benchmark that fails the build on a shared CI runner teaches people to ignore
# it; the performance guard rails that *do* assert live in the `perf` suite in
# examples/.

add_executable(testforge_bench SchedulingBenchmark.cpp)

target_link_libraries(testforge_bench
    PRIVATE
        TestForge::core
        TestForge::warnings
        TestForge::sanitizers
)

target_compile_features(testforge_bench PRIVATE cxx_std_20)

set_target_properties(testforge_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
