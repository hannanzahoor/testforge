# TestForge's own test-suite.
#
# GoogleTest is used here, and deliberately *not* TestForge's own assertions:
# the thing under test cannot also be the thing doing the checking. That is
# also why examples/ uses the ShortAssertions header and this directory does
# not — see include/testforge/testing/ShortAssertions.hpp.

include(GoogleTest)

set(TESTFORGE_TEST_SOURCES
    unit/JsonTest.cpp
    unit/CoreTest.cpp
    unit/TestingTest.cpp
    unit/ExecutionTest.cpp
    unit/NetTest.cpp
    unit/DiagnosticsTest.cpp
    unit/PersistenceTest.cpp
    unit/AiTest.cpp
    unit/ReportingTest.cpp

    integration/RunnerTest.cpp
    integration/HttpTest.cpp

    failure_injection/FailureInjectionTest.cpp
)

add_executable(testforge_tests ${TESTFORGE_TEST_SOURCES})

target_include_directories(testforge_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(testforge_tests
    PRIVATE
        TestForge::core
        TestForge::warnings
        TestForge::sanitizers
        GTest::gtest
        GTest::gtest_main
)

target_compile_features(testforge_tests PRIVATE cxx_std_20)

# Registering each GoogleTest case with CTest individually means `ctest -R` can
# select one, and a crash is attributed to a test rather than to the binary.
# DISCOVERY_MODE PRE_TEST defers enumeration to test time, which keeps
# cross-compiling and sanitizer builds working.
gtest_discover_tests(testforge_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        # Generous: the timeout tests deliberately sleep, and a loaded CI
        # runner is slower than a laptop.
        TIMEOUT 180
        LABELS "unit"
)

# A single target that runs everything, for `cmake --build build --target check`.
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --timeout 180
    DEPENDS testforge_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running the TestForge test-suite"
)
