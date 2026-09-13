/// Tests for the reporters.
///
/// Each reporter is a pure function of a TestRun, which is what makes this
/// file possible: the runs are built by hand, so no tests are executed and
/// nothing touches the network or the database.

#include "testforge/reporting/Reporter.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace testforge;
using namespace testforge::reporting;

namespace {

TestRun buildRun() {
    TestRun run;
    run.runId = "abcdef0123456789";
    run.label = "unit";
    run.startedAt = fromEpochMillis(1757658692417LL);
    run.finishedAt = run.startedAt + Milliseconds{2500};
    run.duration = Milliseconds{2500};
    run.workers = 4;
    run.filterDescription = "--suite api";
    run.hostname = "test-host";
    run.gitCommit = "abc1234";

    TestResult passed = TestResult::make("api", "health", TestStatus::Passed);
    passed.runId = run.runId;
    passed.duration = Milliseconds{12};

    TestResult failed = TestResult::make("api", "create", TestStatus::Failed);
    failed.runId = run.runId;
    failed.failureCategory = FailureCategory::ApplicationFailure;
    failed.duration = Milliseconds{340};
    failed.errorMessage = "unexpected HTTP status for POST /users";
    failed.errorDetail = "Expected:   201\nActual:     500";
    failed.logs = {"sending POST /users", "received 500"};
    failed.metadata.set("http_status", 500);
    failed.metadata.set("http_method", "POST");
    failed.metadata.set("http_url", "http://host/users");
    failed.metadata.set("response_excerpt", R"({"detail":"boom"})");

    TestResult skipped = TestResult::make("gpu", "driver", TestStatus::Skipped);
    skipped.runId = run.runId;
    skipped.errorMessage = "no NVIDIA GPU detected";

    TestResult timedOut = TestResult::make("api", "slow", TestStatus::Timeout);
    timedOut.runId = run.runId;
    timedOut.failureCategory = FailureCategory::Timeout;
    timedOut.duration = Milliseconds{5000};
    timedOut.errorMessage = "test exceeded its 5000ms deadline";

    TestResult errored = TestResult::make("api", "unreachable", TestStatus::Error);
    errored.runId = run.runId;
    errored.failureCategory = FailureCategory::NetworkFailure;
    errored.errorMessage = "connection refused";
    errored.flakyCandidate = true;

    run.results = {passed, failed, skipped, timedOut, errored};
    return run;
}

}  // namespace

// ---------------------------------------------------------------------------
// Statistics — everything the reporters render comes from here
// ---------------------------------------------------------------------------

TEST(RunStatistics, CountsByStatus) {
    const RunStatistics stats = buildRun().statistics();
    EXPECT_EQ(stats.total, 5);
    EXPECT_EQ(stats.passed, 1);
    EXPECT_EQ(stats.failed, 1);
    EXPECT_EQ(stats.skipped, 1);
    EXPECT_EQ(stats.errors, 1);
    EXPECT_EQ(stats.timeouts, 1);
}

TEST(RunStatistics, SuccessRateExcludesSkips) {
    // Counting a skip as a failure would punish a machine for not having a
    // GPU; counting it as a pass would overstate coverage.
    const RunStatistics stats = buildRun().statistics();
    EXPECT_DOUBLE_EQ(stats.successRate(), 25.0);  // 1 passed of 4 considered
}

TEST(RunStatistics, EmptyRunDoesNotDivideByZero) {
    const RunStatistics stats = RunStatistics::compute({});
    EXPECT_EQ(stats.total, 0);
    EXPECT_DOUBLE_EQ(stats.successRate(), 0.0);
    EXPECT_DOUBLE_EQ(stats.parallelEfficiency(), 0.0);
}

TEST(RunStatistics, DurationPercentiles) {
    std::vector<TestResult> results;
    for (int i = 1; i <= 100; ++i) {
        TestResult result = TestResult::make("s", "t" + std::to_string(i), TestStatus::Passed);
        result.duration = Milliseconds{i};
        results.push_back(result);
    }
    const RunStatistics stats = RunStatistics::compute(results);
    EXPECT_EQ(stats.medianDuration.count(), 50);
    EXPECT_EQ(stats.p95Duration.count(), 95);
    EXPECT_EQ(stats.maxDuration.count(), 100);
    EXPECT_EQ(stats.slowestTest, "s.t100");
}

TEST(RunStatistics, ExitCodeDistinguishesFailureFromToolBreakage) {
    TestRun clean;
    clean.results.push_back(TestResult::make("s", "a", TestStatus::Passed));
    EXPECT_EQ(clean.exitCode(), 0);

    TestRun withFailures = clean;
    withFailures.results.push_back(TestResult::make("s", "b", TestStatus::Failed));
    EXPECT_EQ(withFailures.exitCode(), 1);

    // A test hitting a configuration problem is still a test result...
    TestRun withConfigProblem = clean;
    TestResult configProblem = TestResult::make("s", "c", TestStatus::Error);
    configProblem.failureCategory = FailureCategory::ConfigurationFailure;
    withConfigProblem.results.push_back(configProblem);
    EXPECT_EQ(withConfigProblem.exitCode(), 1);

    // ...but a bug in TestForge itself is not.
    TestRun withFrameworkBug = clean;
    TestResult frameworkBug = TestResult::make("s", "d", TestStatus::Error);
    frameworkBug.failureCategory = FailureCategory::FrameworkError;
    withFrameworkBug.results.push_back(frameworkBug);
    EXPECT_EQ(withFrameworkBug.exitCode(), 2);
}

TEST(TestResult, JsonRoundTrip) {
    const TestRun run = buildRun();
    const TestRun again = TestRun::fromJson(run.toJson());

    EXPECT_EQ(again.runId, run.runId);
    EXPECT_EQ(again.workers, run.workers);
    EXPECT_EQ(again.results.size(), run.results.size());
    EXPECT_EQ(again.results[1].failureCategory, run.results[1].failureCategory);
    EXPECT_EQ(again.results[1].logs.size(), run.results[1].logs.size());
    EXPECT_EQ(again.results[1].metadata.at("http_status").asInt(), 500);
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

TEST(ConsoleReporter, RendersTheSummaryAndFailures) {
    ConsoleReporterOptions options;
    options.color = false;
    options.live = false;
    const ConsoleReporter reporter(options);

    const std::string text = reporter.render(buildRun());

    EXPECT_NE(text.find("TestForge Test Report"), std::string::npos);
    EXPECT_NE(text.find("Total     5"), std::string::npos);
    EXPECT_NE(text.find("api.create"), std::string::npos);
    EXPECT_NE(text.find("APPLICATION_FAILURE"), std::string::npos);
    // The category's meaning is spelled out, not just its name.
    EXPECT_NE(text.find("Check its logs"), std::string::npos);
    EXPECT_NE(text.find("RESULT: FAIL"), std::string::npos);
}

TEST(ConsoleReporter, ShowsTheHttpExchangeForApiFailures) {
    ConsoleReporterOptions options;
    options.color = false;
    options.live = false;
    const std::string text = ConsoleReporter(options).render(buildRun());

    EXPECT_NE(text.find("POST http://host/users -> 500"), std::string::npos);
    EXPECT_NE(text.find("boom"), std::string::npos);
}

TEST(ConsoleReporter, FlagsFlakyCandidates) {
    ConsoleReporterOptions options;
    options.color = false;
    options.live = false;
    const std::string text = ConsoleReporter(options).render(buildRun());

    EXPECT_NE(text.find("Flaky candidates"), std::string::npos);
    EXPECT_NE(text.find("heuristic"), std::string::npos) << "the caveat must travel with it";
}

TEST(ConsoleReporter, ColourIsOptional) {
    ConsoleReporterOptions plain;
    plain.color = false;
    plain.live = false;
    EXPECT_EQ(ConsoleReporter(plain).render(buildRun()).find("\033["), std::string::npos);

    ConsoleReporterOptions coloured;
    coloured.color = true;
    coloured.live = false;
    EXPECT_NE(ConsoleReporter(coloured).render(buildRun()).find("\033["), std::string::npos);
}

TEST(ConsoleReporter, PassingRunSaysSo) {
    TestRun run;
    run.runId = "clean";
    run.results.push_back(TestResult::make("s", "a", TestStatus::Passed));

    ConsoleReporterOptions options;
    options.color = false;
    options.live = false;
    const std::string text = ConsoleReporter(options).render(run);
    EXPECT_NE(text.find("RESULT: PASS"), std::string::npos);
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

TEST(JsonReporter, ProducesParseableVersionedOutput) {
    const std::string text = JsonReporter(true).render(buildRun());
    const json::Value document = json::parse(text);

    EXPECT_EQ(document.at("report_format").asString(), "testforge.run");
    EXPECT_EQ(document.at("report_version").asInt(), 1);
    EXPECT_EQ(document.at("run_id").asString(), "abcdef0123456789");
    EXPECT_EQ(document.at("results").size(), 5U);
    EXPECT_EQ(document.path("statistics.failed")->asInt(), 1);
}

TEST(JsonReporter, CompactAndPrettyAgree) {
    const TestRun run = buildRun();
    EXPECT_TRUE(json::parse(JsonReporter(false).render(run)) ==
                json::parse(JsonReporter(true).render(run)));
}

// ---------------------------------------------------------------------------
// HTML
// ---------------------------------------------------------------------------

TEST(HtmlReporter, ProducesASelfContainedDocument) {
    const std::string html = HtmlReporter().render(buildRun());

    EXPECT_NE(html.find("<!doctype html>"), std::string::npos);
    EXPECT_NE(html.find("</html>"), std::string::npos);
    // Self-contained: it must open from a file:// URL with no network.
    EXPECT_EQ(html.find("<script src="), std::string::npos);
    EXPECT_EQ(html.find("<link rel=\"stylesheet\""), std::string::npos);
    EXPECT_EQ(html.find("http://cdn"), std::string::npos);
    EXPECT_NE(html.find("<style>"), std::string::npos);
}

TEST(HtmlReporter, EscapesContentFromTheSystemUnderTest) {
    // A response body is attacker-influenced input; it must not become markup.
    TestRun run = buildRun();
    run.results[1].errorMessage = "<script>alert('xss')</script>";
    run.results[1].errorDetail = "<img src=x onerror=alert(1)>";

    const std::string html = HtmlReporter().render(run);
    EXPECT_EQ(html.find("<script>alert"), std::string::npos);
    EXPECT_EQ(html.find("<img src=x"), std::string::npos);
    EXPECT_NE(html.find("&lt;script&gt;"), std::string::npos);
}

TEST(HtmlReporter, WarnsWhenGpuDataIsMock) {
    TestRun run = buildRun();
    json::Value gpu = json::Value::object();
    gpu.set("is_mock_data", true);
    gpu.set("summary", "[MOCK TEST DATA] 1x MOCK-GPU");
    run.results[1].diagnostics.set("gpu", gpu);

    const std::string html = HtmlReporter().render(run);
    EXPECT_NE(html.find("MOCK TEST DATA"), std::string::npos);
}

TEST(HtmlReporter, ShowsEveryResultAndTheFailureBreakdown) {
    const std::string html = HtmlReporter().render(buildRun());
    for (const char* name :
         {"api.health", "api.create", "gpu.driver", "api.slow", "api.unreachable"}) {
        EXPECT_NE(html.find(name), std::string::npos) << name;
    }
    EXPECT_NE(html.find("Failure categories"), std::string::npos);
}

// ---------------------------------------------------------------------------
// JUnit
// ---------------------------------------------------------------------------

TEST(JUnitReporter, ProducesWellFormedXmlGroupedBySuite) {
    const std::string xml = JUnitReporter().render(buildRun());

    EXPECT_NE(xml.find(R"(<?xml version="1.0" encoding="UTF-8"?>)"), std::string::npos);
    EXPECT_NE(xml.find("<testsuites"), std::string::npos);
    EXPECT_NE(xml.find(R"(<testsuite name="api")"), std::string::npos);
    EXPECT_NE(xml.find(R"(<testsuite name="gpu")"), std::string::npos);
    EXPECT_NE(xml.find("</testsuites>"), std::string::npos);
}

TEST(JUnitReporter, MapsStatusesOntoJUnitElements) {
    const std::string xml = JUnitReporter().render(buildRun());
    EXPECT_NE(xml.find("<failure"), std::string::npos);  // FAILED
    EXPECT_NE(xml.find("<error"), std::string::npos);    // ERROR and TIMEOUT
    EXPECT_NE(xml.find("<skipped"), std::string::npos);  // SKIPPED
}

TEST(JUnitReporter, EscapesXmlMetacharacters) {
    TestRun run = buildRun();
    run.results[1].errorMessage = R"(expected <200> & got "500")";

    const std::string xml = JUnitReporter().render(run);
    EXPECT_NE(xml.find("&lt;200&gt;"), std::string::npos);
    EXPECT_NE(xml.find("&amp;"), std::string::npos);
    EXPECT_EQ(xml.find(R"(message="expected <200>)"), std::string::npos);
}

// ---------------------------------------------------------------------------
// ReportWriter
// ---------------------------------------------------------------------------

TEST(ReportWriter, WritesEveryEnabledFormat) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "testforge-report-test";
    std::filesystem::remove_all(directory);

    ReportingConfig config;
    config.outputDirectory = directory.string();
    config.json = true;
    config.html = true;
    config.junit = true;

    const std::vector<std::string> written = ReportWriter(config).write(buildRun());
    EXPECT_EQ(written.size(), 3U);
    for (const std::string& path : written) {
        EXPECT_TRUE(std::filesystem::exists(path)) << path;
        EXPECT_GT(std::filesystem::file_size(path), 0U) << path;
    }

    // A stable alias makes scripts and the dashboard simpler.
    EXPECT_TRUE(std::filesystem::exists(directory / "latest.json"));
    EXPECT_TRUE(std::filesystem::exists(directory / "latest.html"));

    std::filesystem::remove_all(directory);
}

TEST(ReportWriter, WritesNothingWhenEveryFormatIsOff) {
    ReportingConfig config;
    config.outputDirectory =
        (std::filesystem::temp_directory_path() / "testforge-report-none").string();
    config.json = false;
    config.html = false;
    config.junit = false;

    EXPECT_TRUE(ReportWriter(config).write(buildRun()).empty());
    std::filesystem::remove_all(config.outputDirectory);
}

TEST(ReportWriter, AnUnwritableDirectoryIsLoggedNotThrown) {
    // Losing a report must never turn a passing run into a failing one.
    ReportingConfig config;
    config.outputDirectory = "/proc/testforge-cannot-create-this";
    config.json = true;

    std::vector<std::string> written;
    EXPECT_NO_THROW(written = ReportWriter(config).write(buildRun()));
    EXPECT_TRUE(written.empty());
}

TEST(ReportWriter, FilenamesAreFilesystemSafe) {
    ReportingConfig config;
    config.outputDirectory = "reports";
    const std::string path = ReportWriter(config).pathFor(buildRun(), JsonReporter());

    // No colons: they are illegal on Windows and awkward everywhere.
    EXPECT_EQ(path.find(':'), std::string::npos);
    EXPECT_NE(path.find("run-abcdef01"), std::string::npos);
    EXPECT_NE(path.find(".json"), std::string::npos);
}
