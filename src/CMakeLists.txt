# ---------------------------------------------------------------------------
# testforge_core — the engine, as a static library
#
# Everything except main() lives here, which is what lets the CLI, the REST
# server, the benchmarks and the unit tests all link against the same code.
# ---------------------------------------------------------------------------

set(TESTFORGE_CORE_SOURCES
    core/Clock.cpp
    core/Config.cpp
    core/CoreTypes.cpp
    core/CancellationToken.cpp
    core/Environment.cpp
    core/Ids.cpp
    core/Interrupt.cpp
    core/Json.cpp
    core/Logger.cpp
    core/Process.cpp
    core/Status.cpp
    core/StringUtils.cpp
    core/TestContext.cpp
    core/TestResult.cpp

    testing/Assertions.cpp
    testing/ApiTestCase.cpp
    testing/SpecTestCase.cpp
    testing/TestRegistry.cpp
    testing/TestSelector.cpp

    execution/FailureClassifier.cpp
    execution/TestRunner.cpp
    execution/ThreadPool.cpp

    net/HttpClient.cpp
    net/HttpServer.cpp
    net/HttpTypes.cpp
    net/Url.cpp

    diagnostics/DiagnosticCollector.cpp
    diagnostics/GpuProviders.cpp
    diagnostics/LinuxDiagnosticProvider.cpp
    diagnostics/SystemInfo.cpp

    persistence/Database.cpp
    persistence/Schema.cpp
    persistence/SqliteResultRepository.cpp

    reporting/ConsoleReporter.cpp
    reporting/HtmlReporter.cpp
    reporting/JsonReporter.cpp
    reporting/JUnitReporter.cpp
    reporting/ReportWriter.cpp

    ai/AiProvider.cpp
    ai/FailureContext.cpp
    ai/HttpAiProvider.cpp
    ai/MockAiProvider.cpp
    ai/SpecValidator.cpp
    ai/TestSpec.cpp

    api/RestApiServer.cpp
    api/TestForgeService.cpp
)

add_library(testforge_core STATIC ${TESTFORGE_CORE_SOURCES})
add_library(TestForge::core ALIAS testforge_core)

target_include_directories(testforge_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(testforge_core
    PUBLIC
        Threads::Threads
    PRIVATE
        TestForge::sqlite3
        TestForge::warnings
        TestForge::sanitizers
)

target_compile_features(testforge_core PUBLIC cxx_std_20)

if(TESTFORGE_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set_target_properties(testforge_core PROPERTIES
            CXX_CLANG_TIDY "${CLANG_TIDY_EXE};--quiet")
    else()
        message(WARNING "TESTFORGE_ENABLE_CLANG_TIDY is ON but clang-tidy was not found")
    endif()
endif()

# ---------------------------------------------------------------------------
# testforge — the command line interface
# ---------------------------------------------------------------------------

set(TESTFORGE_CLI_SOURCES
    cli/main.cpp
    cli/CommandLine.cpp
    cli/Commands.cpp
    cli/CliOutput.cpp
)

# The bundled suites are compiled into the binary so that a single executable
# carries its own tests. examples/CMakeLists.txt explains why they are an
# OBJECT library rather than a static one.
if(TESTFORGE_BUILD_EXAMPLES)
    list(APPEND TESTFORGE_CLI_SOURCES $<TARGET_OBJECTS:testforge_example_tests>)
endif()

add_executable(testforge ${TESTFORGE_CLI_SOURCES})

target_link_libraries(testforge
    PRIVATE
        TestForge::core
        TestForge::warnings
        TestForge::sanitizers
)

set_target_properties(testforge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
