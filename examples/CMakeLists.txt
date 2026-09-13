# The bundled test suites.
#
# An OBJECT library rather than a static one, on purpose. These translation
# units contain nothing the linker can see a reference to: each test registers
# itself from a namespace-scope constructor. Object files are always linked in
# whole, so the registrations survive; a static archive would have its unused
# members discarded and `testforge list` would print nothing.
#
# The alternative is --whole-archive, which is toolchain-specific. This is not.

add_library(testforge_example_tests OBJECT
    sample_tests/SmokeTests.cpp
    sample_tests/ApiTests.cpp
    sample_tests/SystemTests.cpp
    sample_tests/GpuTests.cpp
    sample_tests/PerformanceTests.cpp
    sample_tests/FailureInjectionTests.cpp
)

target_link_libraries(testforge_example_tests
    PRIVATE
        TestForge::core
        TestForge::warnings
        TestForge::sanitizers
)

target_compile_features(testforge_example_tests PRIVATE cxx_std_20)

# The suites are linked into the `testforge` binary (see src/CMakeLists.txt),
# which is what makes a single static executable carry its own tests.
