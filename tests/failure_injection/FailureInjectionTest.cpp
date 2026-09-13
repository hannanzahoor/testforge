/// Failure-injection tests for the framework itself.
///
/// The bundled `failure_injection` suite in examples/ demonstrates the triage
/// pipeline to a human. This file *asserts* on it: for each injected fault it
/// checks that the runner produced the right status, the right category, the
/// right evidence, and a persisted record — the properties a demo shows but
/// does not prove.
///
/// Together they answer the question a test tool has to answer about itself:
/// when something breaks, does it notice, and does it say something useful?

#include "../fixtures/InMemoryRepository.hpp"

#include "testforge/ai/FailureContext.hpp"
#include "testforge/ai/MockAiProvider.hpp"
#include "testforge/core/Exceptions.hpp"
#include "testforge/core/Process.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/diagnostics/DiagnosticProvider.hpp"
#include "testforge/diagnostics/GpuProviders.hpp"
#include "testforge/execution/TestRunner.hpp"
#include "testforge/net/HttpClient.hpp"
#include "testforge/reporting/Reporter.hpp"
#include "testforge/testing/Assertions.hpp"
#include "testforge/testing/TestRegistry.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace testforge;
using testforge::testing::InMemoryRepository;

namespace {

class InjectionFixture : public ::testing::Test {
 protected:
    void SetUp() override {
        TestRegistry::instance().clear();
        config.database.enabled = false;
        config.reporting.console = false;
        config.reporting.json = false;
        config.reporting.html = false;
        config.execution.defaultTimeoutMs = 4000;
        config.execution.cancellationGraceMs = 400;
        config.diagnostics.collectOnFailure = true;

        repository = std::make_shared<InMemoryRepository>();
        collector = diagnostics::DiagnosticCollector::createDefault(config.diagnostics);
        // A mock GPU so the diagnostics blob has a GPU section on any machine.
        collector->setGpuProvider(std::make_shared<diagnostics::MockGpuProvider>(1));
    }

    void TearDown() override { TestRegistry::instance().clear(); }

    void inject(const std::string& name,
                std::function<void(TestContext&)> body,
                std::int64_t timeoutMs = 0) {
        TestMetadata metadata;
        metadata.suite = "injected";
        metadata.name = name;
        metadata.timeoutMs = timeoutMs;
        metadata.tags = {"injected"};

        TestRegistry::instance().registerTest(metadata, [metadata, body]() -> TestCasePtr {
            return std::make_unique<FunctionTestCase>(metadata, body);
        });
    }

    TestResult runOne(const std::string& name) {
        TestRunner runner(config, TestRegistry::instance());
        runner.setRepository(repository);
        runner.setDiagnosticCollector(collector);

        RunOptions options;
        options.filter.names = {"injected." + name};
        const TestRun result = runner.run(options);
        EXPECT_EQ(result.results.size(), 1U) << "expected exactly one result for " << name;
        return result.results.empty() ? TestResult{} : result.results.front();
    }

    Config config;
    std::shared_ptr<InMemoryRepository> repository;
    std::shared_ptr<diagnostics::DiagnosticCollector> collector;
};

}  // namespace

// ---------------------------------------------------------------------------
// Each fault is detected and classified correctly
// ---------------------------------------------------------------------------

TEST_F(InjectionFixture, AssertionFailure) {
    inject("assertion", [](TestContext&) { TF_ASSERT_EQ(6 * 7, 43); });

    const TestResult result = runOne("assertion");
    EXPECT_EQ(result.status, TestStatus::Failed);
    EXPECT_EQ(result.failureCategory, FailureCategory::AssertionFailure);
    // The evidence a developer actually needs.
    EXPECT_NE(result.errorDetail.find("42"), std::string::npos);
    EXPECT_NE(result.errorDetail.find("43"), std::string::npos);
    EXPECT_NE(result.errorDetail.find("FailureInjectionTest.cpp"), std::string::npos);
}

TEST_F(InjectionFixture, NetworkFailure) {
    inject("network", [](TestContext& ctx) {
        net::HttpClientOptions options;
        options.connectTimeout = Milliseconds{1000};
        options.policy.blockLoopback = false;
        net::HttpClient client(options);

        const net::HttpResponse response = client.get("http://127.0.0.1:9/nothing");
        ctx.addMetadata("transport_error", response.errorMessage);
        throw NetworkError("unreachable: " + response.errorMessage);
    });

    const TestResult result = runOne("network");
    EXPECT_EQ(result.status, TestStatus::Error);
    EXPECT_EQ(result.failureCategory, FailureCategory::NetworkFailure);
}

TEST_F(InjectionFixture, ApplicationFailureIsPromotedFromAnAssertion) {
    // The refinement pass: a failed assertion against a 5xx is the service's
    // fault, not the test's, and the category has to say so.
    inject("application", [](TestContext& ctx) {
        ctx.addMetadata("http_status", 503);
        ctx.addMetadata("http_url", "http://host/boom");
        TF_ASSERT_EQ(503, 200);
    });

    const TestResult result = runOne("application");
    EXPECT_EQ(result.status, TestStatus::Failed);
    EXPECT_EQ(result.failureCategory, FailureCategory::ApplicationFailure);
}

TEST_F(InjectionFixture, ClientErrorStaysAnAssertionFailure) {
    inject("client_error", [](TestContext& ctx) {
        ctx.addMetadata("http_status", 404);
        TF_ASSERT_EQ(404, 200);
    });

    // 4xx is not the server misbehaving, so it must not be promoted.
    EXPECT_EQ(runOne("client_error").failureCategory, FailureCategory::AssertionFailure);
}

TEST_F(InjectionFixture, DependencyFailure) {
    inject("dependency", [](TestContext& ctx) {
        const ProcessResult process = ProcessRunner::run("testforge-not-installed", {"-v"});
        ctx.addMetadata("missing_dependency", "testforge-not-installed");
        throw DependencyError("missing tool: " + process.launchError);
    });

    const TestResult result = runOne("dependency");
    EXPECT_EQ(result.status, TestStatus::Error);
    EXPECT_EQ(result.failureCategory, FailureCategory::DependencyFailure);
}

TEST_F(InjectionFixture, EnvironmentFailure) {
    inject("environment", [](TestContext&) {
        throw EnvironmentError("cannot write to /proc: permission denied");
    });

    EXPECT_EQ(runOne("environment").failureCategory, FailureCategory::EnvironmentFailure);
}

TEST_F(InjectionFixture, ResourceFailure) {
    inject("resource", [](TestContext&) { throw ResourceError("cannot allocate 4 GiB"); });
    EXPECT_EQ(runOne("resource").failureCategory, FailureCategory::ResourceFailure);
}

TEST_F(InjectionFixture, ConfigurationFailure) {
    inject("configuration",
           [](TestContext&) { throw ConfigurationError("required setting is absent"); });
    EXPECT_EQ(runOne("configuration").failureCategory, FailureCategory::ConfigurationFailure);
}

TEST_F(InjectionFixture, CooperativeTimeout) {
    inject(
        "cooperative",
        [](TestContext& ctx) {
            for (int i = 0; i < 200; ++i) {
                ctx.throwIfCancelled();
                (void)ctx.sleepFor(Milliseconds{20});
            }
        },
        200);

    const TestResult result = runOne("cooperative");
    EXPECT_EQ(result.status, TestStatus::Timeout);
    EXPECT_EQ(result.failureCategory, FailureCategory::Timeout);
}

TEST_F(InjectionFixture, UncooperativeTimeout) {
    inject(
        "uncooperative",
        [](TestContext&) { std::this_thread::sleep_for(std::chrono::milliseconds(1200)); },
        150);

    const TestResult result = runOne("uncooperative");
    EXPECT_EQ(result.status, TestStatus::Timeout);
    EXPECT_EQ(result.failureCategory, FailureCategory::Timeout);
    // The report has to explain what to do about it.
    EXPECT_NE(result.errorDetail.find("throwIfCancelled"), std::string::npos);
}

TEST_F(InjectionFixture, UnexpectedStdException) {
    inject("std_exception", [](TestContext&) { throw std::out_of_range("index 12 out of range"); });

    const TestResult result = runOne("std_exception");
    EXPECT_EQ(result.status, TestStatus::Error);
    EXPECT_NE(result.errorMessage.find("index 12"), std::string::npos);
}

TEST_F(InjectionFixture, NonStdExceptionIsSurvivable) {
    inject("non_std", [](TestContext&) { throw 42; });  // NOLINT

    const TestResult result = runOne("non_std");
    EXPECT_EQ(result.status, TestStatus::Error);
    EXPECT_EQ(result.failureCategory, FailureCategory::Unknown);
    EXPECT_FALSE(result.errorDetail.empty()) << "the report should say what happened";
}

// ---------------------------------------------------------------------------
// The pipeline around the failure
// ---------------------------------------------------------------------------

TEST_F(InjectionFixture, FailuresCarryDiagnosticsAndPassesDoNot) {
    inject("fails", [](TestContext&) { TF_ASSERT_TRUE(false); });
    inject("passes", [](TestContext&) {});

    const TestResult failed = runOne("fails");
    ASSERT_FALSE(failed.diagnostics.empty());
    EXPECT_TRUE(failed.diagnostics.contains("system"));
    EXPECT_TRUE(failed.diagnostics.contains("gpu"));
    // The mock flag must be visible wherever the data goes.
    EXPECT_TRUE(failed.diagnostics.path("gpu.is_mock_data")->asBool());

    EXPECT_TRUE(runOne("passes").diagnostics.empty());
}

TEST_F(InjectionFixture, FailuresAreCapturedWithTheirLogs) {
    inject("logged", [](TestContext& ctx) {
        ctx.log().info("step one");
        ctx.log().warn("step two looked wrong");
        TF_ASSERT_TRUE(false);
    });

    const TestResult result = runOne("logged");
    ASSERT_GE(result.logs.size(), 2U);

    bool sawTheWarning = false;
    for (const std::string& line : result.logs) {
        sawTheWarning = sawTheWarning || line.find("step two") != std::string::npos;
    }
    EXPECT_TRUE(sawTheWarning);
}

TEST_F(InjectionFixture, FailuresArePersisted) {
    inject("persisted", [](TestContext&) { TF_ASSERT_TRUE(false); });
    const TestResult result = runOne("persisted");

    const std::vector<TestResult> stored = repository->loadResults(result.runId);
    ASSERT_EQ(stored.size(), 1U);
    EXPECT_EQ(stored[0].status, TestStatus::Failed);
    EXPECT_EQ(stored[0].failureCategory, FailureCategory::AssertionFailure);
}

TEST_F(InjectionFixture, FailuresRenderInEveryReportFormat) {
    inject("reported", [](TestContext& ctx) {
        ctx.addMetadata("http_status", 500);
        TF_ASSERT_EQ(500, 200);
    });

    TestRun run;
    run.runId = "report-run";
    run.results = {runOne("reported")};

    reporting::ConsoleReporterOptions consoleOptions;
    consoleOptions.color = false;
    consoleOptions.live = false;

    const std::string console = reporting::ConsoleReporter(consoleOptions).render(run);
    EXPECT_NE(console.find("injected.reported"), std::string::npos);
    EXPECT_NE(console.find("APPLICATION_FAILURE"), std::string::npos);

    const json::Value document = json::parse(reporting::JsonReporter().render(run));
    EXPECT_EQ(document.path("statistics.failed")->asInt(), 1);

    const std::string html = reporting::HtmlReporter().render(run);
    EXPECT_NE(html.find("injected.reported"), std::string::npos);

    const std::string xml = reporting::JUnitReporter().render(run);
    EXPECT_NE(xml.find("<failure"), std::string::npos);
}

TEST_F(InjectionFixture, FailureContextIsUsableByTheAiLayer) {
    inject("analysed", [](TestContext& ctx) {
        ctx.addMetadata("http_status", 500);
        ctx.addMetadata("http_url", "http://host/boom");
        ctx.log().error("the service returned 500");
        TF_ASSERT_EQ(500, 200);
    });

    const TestResult result = runOne("analysed");

    // The last step of the triage pipeline: evidence in, advisory analysis out.
    const json::Value context = ai::FailureContext{}.build(result);
    EXPECT_EQ(context.path("result.metadata.http_status")->asInt(), 500);

    ai::MockAiProvider provider;
    ai::AnalysisRequest request;
    request.context = context;
    request.testName = result.qualifiedName();
    request.failureCategory = std::string(toString(result.failureCategory));

    const ai::AnalysisResult analysis = provider.analyseFailure(request);
    ASSERT_TRUE(analysis.ok);
    EXPECT_FALSE(analysis.probableCause.empty());
    EXPECT_FALSE(analysis.evidence.empty());

    // And the verdict is still the engine's, not the model's.
    EXPECT_EQ(result.status, TestStatus::Failed);
    EXPECT_TRUE(analysis.toJson().at("advisory_only").asBool());
}

TEST_F(InjectionFixture, TheWholeInjectedSuiteRunsWithoutTakingTheProcessDown) {
    // Everything at once, in parallel: the run must complete and report each
    // fault rather than crashing, hanging, or losing results.
    inject("a_assertion", [](TestContext&) { TF_ASSERT_TRUE(false); });
    inject("b_network", [](TestContext&) { throw NetworkError("refused"); });
    inject("c_rogue", [](TestContext&) { throw 7; });  // NOLINT
    inject(
        "d_timeout",
        [](TestContext&) { std::this_thread::sleep_for(std::chrono::milliseconds(800)); },
        100);
    inject("e_resource", [](TestContext&) { throw ResourceError("oom"); });
    inject("f_passes", [](TestContext&) {});

    TestRunner runner(config, TestRegistry::instance());
    runner.setRepository(repository);
    runner.setDiagnosticCollector(collector);

    RunOptions options;
    options.workers = 4;
    const TestRun run = runner.run(options);

    EXPECT_EQ(run.results.size(), 6U);
    const RunStatistics stats = run.statistics();
    EXPECT_EQ(stats.passed, 1);
    EXPECT_EQ(stats.failed, 1);
    EXPECT_EQ(stats.timeouts, 1);
    EXPECT_EQ(stats.errors, 3);

    // Exit code 1: tests failed. Not 2, which would mean TestForge is broken.
    EXPECT_EQ(run.exitCode(), 1);
    EXPECT_EQ(repository->loadResults(run.runId).size(), 6U);
}
