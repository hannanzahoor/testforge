/// End-to-end tests of the execution engine.
///
/// These drive a real TestRunner over a real registry with real threads, and
/// assert on the TestResult it produces. The repository is the in-memory fake,
/// which is the point of the ResultRepository interface: the engine can be
/// exercised completely with no database, no schema and no file system.

#include "../fixtures/InMemoryRepository.hpp"

#include "testforge/core/Exceptions.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/diagnostics/DiagnosticProvider.hpp"
#include "testforge/diagnostics/GpuProviders.hpp"
#include "testforge/execution/TestRunner.hpp"
#include "testforge/testing/Assertions.hpp"
#include "testforge/testing/TestRegistry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

using namespace testforge;
using testforge::testing::InMemoryRepository;

namespace {

/// A private registry for each test, so nothing leaks between them and the
/// process-wide catalogue of example suites is untouched.
class RunnerFixture : public ::testing::Test {
 protected:
    void SetUp() override {
        TestRegistry::instance().clear();
        config.database.enabled = false;
        config.reporting.console = false;
        config.reporting.json = false;
        config.reporting.html = false;
        config.diagnostics.collectOnFailure = false;
        config.execution.defaultTimeoutMs = 5000;
        config.execution.cancellationGraceMs = 500;
    }

    void TearDown() override { TestRegistry::instance().clear(); }

    void add(const std::string& suite,
             const std::string& name,
             std::function<void(TestContext&)> body,
             std::int64_t timeoutMs = 0,
             std::vector<std::string> tags = {}) {
        TestMetadata metadata;
        metadata.suite = suite;
        metadata.name = name;
        metadata.timeoutMs = timeoutMs;
        metadata.tags = std::move(tags);
        metadata.tags.push_back(suite);

        TestRegistry::instance().registerTest(metadata, [metadata, body]() -> TestCasePtr {
            return std::make_unique<FunctionTestCase>(metadata, body);
        });
    }

    TestRun run(RunOptions options = {}) {
        TestRunner runner(config, TestRegistry::instance());
        if (repository) {
            runner.setRepository(repository);
        }
        if (collector) {
            runner.setDiagnosticCollector(collector);
        }
        return runner.run(options);
    }

    [[nodiscard]] static const TestResult* find(const TestRun& testRun,
                                                const std::string& qualifiedName) {
        for (const TestResult& result : testRun.results) {
            if (result.qualifiedName() == qualifiedName) {
                return &result;
            }
        }
        return nullptr;
    }

    Config config;
    std::shared_ptr<InMemoryRepository> repository;
    std::shared_ptr<diagnostics::DiagnosticCollector> collector;
};

}  // namespace

// ---------------------------------------------------------------------------
// Outcomes
// ---------------------------------------------------------------------------

TEST_F(RunnerFixture, RunsAPassingTest) {
    add("s", "ok", [](TestContext&) { TF_ASSERT_TRUE(true); });

    const TestRun result = run();
    ASSERT_EQ(result.results.size(), 1U);
    EXPECT_EQ(result.results[0].status, TestStatus::Passed);
    EXPECT_EQ(result.results[0].failureCategory, FailureCategory::None);
    EXPECT_EQ(result.exitCode(), 0);
    EXPECT_FALSE(result.runId.empty());
}

TEST_F(RunnerFixture, EveryOutcomeIsRepresented) {
    add("s", "pass", [](TestContext&) {});
    add("s", "fail", [](TestContext&) { TF_ASSERT_EQ(1, 2); });
    add("s", "skip", [](TestContext& ctx) { ctx.skip("not applicable here"); });
    add("s", "error", [](TestContext&) { throw NetworkError("refused"); });
    add(
        "s",
        "timeout",
        [](TestContext& ctx) {
            for (int i = 0; i < 200; ++i) {
                ctx.throwIfCancelled();
                (void)ctx.sleepFor(Milliseconds{25});
            }
        },
        200);

    const TestRun result = run();
    ASSERT_EQ(result.results.size(), 5U);

    EXPECT_EQ(find(result, "s.pass")->status, TestStatus::Passed);
    EXPECT_EQ(find(result, "s.fail")->status, TestStatus::Failed);
    EXPECT_EQ(find(result, "s.skip")->status, TestStatus::Skipped);
    EXPECT_EQ(find(result, "s.error")->status, TestStatus::Error);
    EXPECT_EQ(find(result, "s.timeout")->status, TestStatus::Timeout);

    const RunStatistics stats = result.statistics();
    EXPECT_EQ(stats.passed, 1);
    EXPECT_EQ(stats.failed, 1);
    EXPECT_EQ(stats.skipped, 1);
    EXPECT_EQ(stats.errors, 1);
    EXPECT_EQ(stats.timeouts, 1);
}

TEST_F(RunnerFixture, OneFailingTestDoesNotStopTheRest) {
    // The single most important property of a test runner.
    for (int i = 0; i < 10; ++i) {
        const bool shouldFail = i % 3 == 0;
        add("s", "t" + std::to_string(i), [shouldFail](TestContext&) {
            if (shouldFail) {
                throw std::runtime_error("deliberate");
            }
        });
    }

    const TestRun result = run();
    EXPECT_EQ(result.results.size(), 10U);
    EXPECT_EQ(result.statistics().errors, 4);
    EXPECT_EQ(result.statistics().passed, 6);
}

TEST_F(RunnerFixture, ANonStdExceptionCannotKillTheRun) {
    add("s", "rogue", [](TestContext&) { throw 42; });  // NOLINT
    add("s", "after", [](TestContext&) {});

    const TestRun result = run();
    EXPECT_EQ(result.results.size(), 2U);
    EXPECT_EQ(find(result, "s.rogue")->status, TestStatus::Error);
    EXPECT_EQ(find(result, "s.after")->status, TestStatus::Passed);
}

TEST_F(RunnerFixture, ResultsCarryTimingAndIdentity) {
    add("s", "timed", [](TestContext& ctx) { (void)ctx.sleepFor(Milliseconds{40}); });

    const TestRun outcome = run();
    ASSERT_FALSE(outcome.results.empty());
    const TestResult& result = outcome.results.front();
    EXPECT_EQ(result.suiteName, "s");
    EXPECT_EQ(result.testName, "timed");
    EXPECT_FALSE(result.testId.empty());
    EXPECT_FALSE(result.runId.empty());
    EXPECT_GE(result.duration.count(), 30);
    EXPECT_LE(result.startTime, result.endTime);
    EXPECT_FALSE(result.worker.empty());
}

TEST_F(RunnerFixture, TestSuppliedMetadataAndTagsReachTheResult) {
    add("s",
        "annotated",
        [](TestContext& ctx) {
            ctx.addMetadata("custom_field", "custom value");
            ctx.addMetadata("count", 7);
            ctx.addTag("extra");
            ctx.addNote("something worth knowing");
            TF_ASSERT_EQ(1, 2);
        },
        0,
        {"declared"});

    const TestRun outcome = run();
    ASSERT_FALSE(outcome.results.empty());
    const TestResult& result = outcome.results.front();
    EXPECT_EQ(result.metadata.at("custom_field").asString(), "custom value");
    EXPECT_EQ(result.metadata.at("count").asInt(), 7);
    EXPECT_NE(std::find(result.tags.begin(), result.tags.end(), "declared"), result.tags.end());
    EXPECT_NE(std::find(result.tags.begin(), result.tags.end(), "extra"), result.tags.end());
    EXPECT_NE(result.errorDetail.find("something worth knowing"), std::string::npos);
}

TEST_F(RunnerFixture, LogsAreCapturedPerTest) {
    add("s", "chatty", [](TestContext& ctx) {
        ctx.log().info("first");
        ctx.log().warn("second");
        TF_ASSERT_TRUE(false);
    });
    add("s", "quiet", [](TestContext&) {});

    const TestRun result = run();
    const TestResult* chatty = find(result, "s.chatty");
    ASSERT_NE(chatty, nullptr);
    EXPECT_GE(chatty->logs.size(), 2U);

    // Per-test attribution: one test's logs must not appear on another.
    for (const std::string& line : find(result, "s.quiet")->logs) {
        EXPECT_EQ(line.find("first"), std::string::npos);
    }
}

TEST_F(RunnerFixture, DisabledTestsAreSkippedNotRun) {
    TestMetadata metadata;
    metadata.suite = "s";
    metadata.name = "disabled";
    metadata.enabled = false;
    metadata.disabledReason = "waiting on a fix";

    std::atomic<bool> ran{false};
    TestRegistry::instance().registerTest(metadata, [metadata, &ran]() -> TestCasePtr {
        return std::make_unique<FunctionTestCase>(metadata,
                                                  [&ran](TestContext&) { ran.store(true); });
    });

    RunOptions options;
    options.filter.includeDisabled = true;
    const TestRun result = run(options);

    ASSERT_EQ(result.results.size(), 1U);
    EXPECT_EQ(result.results[0].status, TestStatus::Skipped);
    EXPECT_NE(result.results[0].errorMessage.find("waiting on a fix"), std::string::npos);
    EXPECT_FALSE(ran.load());
}

// ---------------------------------------------------------------------------
// Timeouts
// ---------------------------------------------------------------------------

TEST_F(RunnerFixture, ACooperativeTestStopsPromptly) {
    add(
        "s",
        "cooperative",
        [](TestContext& ctx) {
            for (int i = 0; i < 500; ++i) {
                ctx.throwIfCancelled();
                (void)ctx.sleepFor(Milliseconds{10});
            }
        },
        200);

    const Stopwatch watch;
    const TestRun result = run();
    const Milliseconds elapsed = watch.elapsed();

    EXPECT_EQ(result.results[0].status, TestStatus::Timeout);
    EXPECT_EQ(result.results[0].failureCategory, FailureCategory::Timeout);
    // The whole point of cooperation: it stops near the deadline, not after
    // its full five seconds of sleeping.
    EXPECT_LT(elapsed.count(), 2000);
}

TEST_F(RunnerFixture, AnUncooperativeTestIsStillReportedAsTimedOut) {
    // Sleeps without any cancellation point. TestForge refuses to kill the
    // thread, so it stops waiting, records TIMEOUT, and moves on.
    add(
        "s",
        "uncooperative",
        [](TestContext&) { std::this_thread::sleep_for(std::chrono::milliseconds(900)); },
        150);

    const TestRun result = run();
    EXPECT_EQ(result.results[0].status, TestStatus::Timeout);
    EXPECT_EQ(result.results[0].failureCategory, FailureCategory::Timeout);
    EXPECT_TRUE(result.results[0].metadata.contains("deadline_exceeded"));
}

TEST_F(RunnerFixture, FinishingInsideTheGracePeriodIsStillATimeout) {
    // Regression: the grace period is for a cancelled test to unwind, not for
    // an uncooperative one to succeed late. An earlier version reported PASS.
    config.execution.cancellationGraceMs = 2000;
    add(
        "s",
        "late",
        [](TestContext&) { std::this_thread::sleep_for(std::chrono::milliseconds(400)); },
        100);

    const TestRun result = run();
    EXPECT_EQ(result.results[0].status, TestStatus::Timeout);
    EXPECT_NE(result.results[0].errorMessage.find("deadline"), std::string::npos);
}

TEST_F(RunnerFixture, ZeroTimeoutMeansUnbounded) {
    config.execution.defaultTimeoutMs = 0;
    add("s", "slow", [](TestContext& ctx) { (void)ctx.sleepFor(Milliseconds{120}); });

    EXPECT_EQ(run().results[0].status, TestStatus::Passed);
}

TEST_F(RunnerFixture, PerTestTimeoutOverridesTheDefault) {
    config.execution.defaultTimeoutMs = 10000;
    add(
        "s",
        "strict",
        [](TestContext& ctx) {
            for (int i = 0; i < 100; ++i) {
                ctx.throwIfCancelled();
                (void)ctx.sleepFor(Milliseconds{20});
            }
        },
        150);

    const TestRun result = run();
    EXPECT_EQ(result.results[0].status, TestStatus::Timeout);
    EXPECT_EQ(result.results[0].metadata.at("timeout_ms").asInt(), 150);
}

// ---------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------

TEST_F(RunnerFixture, ParallelExecutionUsesSeveralWorkers) {
    constexpr int kTests = 8;
    std::mutex mutex;
    std::set<std::string> workers;

    for (int i = 0; i < kTests; ++i) {
        add("s", "t" + std::to_string(i), [&mutex, &workers](TestContext& ctx) {
            {
                const std::lock_guard<std::mutex> lock(mutex);
                workers.insert(ctx.worker());
            }
            (void)ctx.sleepFor(Milliseconds{60});
        });
    }

    RunOptions options;
    options.workers = 4;
    const TestRun result = run(options);

    EXPECT_EQ(result.results.size(), static_cast<std::size_t>(kTests));
    EXPECT_GT(workers.size(), 1U) << "the run did not actually parallelise";
    EXPECT_EQ(result.workers, 4);
}

TEST_F(RunnerFixture, ParallelIsFasterThanSequentialForSleepingTests) {
    constexpr int kTests = 8;
    for (int i = 0; i < kTests; ++i) {
        add("s", "t" + std::to_string(i), [](TestContext& ctx) {
            (void)ctx.sleepFor(Milliseconds{100});
        });
    }

    RunOptions sequential;
    sequential.workers = 1;
    const Stopwatch sequentialWatch;
    (void)run(sequential);
    const std::int64_t sequentialMs = sequentialWatch.elapsed().count();

    RunOptions parallel;
    parallel.workers = 4;
    const Stopwatch parallelWatch;
    (void)run(parallel);
    const std::int64_t parallelMs = parallelWatch.elapsed().count();

    // Deliberately loose: this asserts that parallelism happens at all, not a
    // particular speed-up, which would be flaky on a loaded CI machine.
    EXPECT_LT(parallelMs, sequentialMs)
        << "sequential " << sequentialMs << "ms vs parallel " << parallelMs << "ms";
}

TEST_F(RunnerFixture, ResultsAreOrderedDeterministicallyRegardlessOfCompletion) {
    // Reverse-ordered sleeps, so completion order is the opposite of name
    // order. The report must still be sorted.
    for (int i = 0; i < 6; ++i) {
        const int delay = (6 - i) * 20;
        add("s", "t" + std::to_string(i), [delay](TestContext& ctx) {
            (void)ctx.sleepFor(Milliseconds{delay});
        });
    }

    RunOptions options;
    options.workers = 6;
    const TestRun result = run(options);

    for (std::size_t i = 1; i < result.results.size(); ++i) {
        EXPECT_LT(result.results[i - 1].qualifiedName(), result.results[i].qualifiedName());
    }
}

TEST_F(RunnerFixture, EachTestGetsItsOwnInstance) {
    // A retry must not inherit state from the attempt that failed.
    std::atomic<int> constructions{0};

    TestMetadata metadata;
    metadata.suite = "s";
    metadata.name = "counted";
    TestRegistry::instance().registerTest(metadata, [metadata, &constructions]() -> TestCasePtr {
        constructions.fetch_add(1);
        return std::make_unique<FunctionTestCase>(metadata, [](TestContext&) {});
    });

    (void)run();
    (void)run();
    EXPECT_EQ(constructions.load(), 2);
}

// ---------------------------------------------------------------------------
// Retries and fail-fast
// ---------------------------------------------------------------------------

TEST_F(RunnerFixture, TransientFailuresAreRetriedAndFlagged) {
    std::atomic<int> attempts{0};
    add("s", "transient", [&attempts](TestContext&) {
        if (attempts.fetch_add(1) == 0) {
            throw NetworkError("connection refused");  // retryable
        }
    });

    RunOptions options;
    options.retryFailed = 2;
    const TestRun result = run(options);

    EXPECT_EQ(result.results[0].status, TestStatus::Passed);
    EXPECT_EQ(result.results[0].attempt, 2);
    // Passing only after a retry is exactly the signature of a flaky test.
    EXPECT_TRUE(result.results[0].flakyCandidate);
    EXPECT_EQ(attempts.load(), 2);
}

TEST_F(RunnerFixture, DeterministicFailuresAreNotRetried) {
    std::atomic<int> attempts{0};
    add("s", "deterministic", [&attempts](TestContext&) {
        attempts.fetch_add(1);
        TF_ASSERT_EQ(1, 2);
    });

    RunOptions options;
    options.retryFailed = 3;
    const TestRun result = run(options);

    EXPECT_EQ(result.results[0].status, TestStatus::Failed);
    // Retrying a broken assertion wastes time and hides the signal.
    EXPECT_EQ(attempts.load(), 1);
    EXPECT_FALSE(result.results[0].flakyCandidate);
}

TEST_F(RunnerFixture, FailFastStopsSchedulingFurtherTests) {
    std::atomic<int> executed{0};
    add("s", "aaa_fails", [&executed](TestContext&) {
        executed.fetch_add(1);
        throw std::runtime_error("stop here");
    });
    for (int i = 0; i < 20; ++i) {
        add("s", "zzz" + std::to_string(i), [&executed](TestContext& ctx) {
            executed.fetch_add(1);
            (void)ctx.sleepFor(Milliseconds{30});
        });
    }

    RunOptions options;
    options.failFast = true;
    options.workers = 1;
    (void)run(options);

    EXPECT_LT(executed.load(), 21) << "fail-fast did not stop scheduling";
}

// ---------------------------------------------------------------------------
// Persistence and diagnostics
// ---------------------------------------------------------------------------

TEST_F(RunnerFixture, ResultsArePersistedAsTheyFinish) {
    repository = std::make_shared<InMemoryRepository>();
    for (int i = 0; i < 5; ++i) {
        add("s", "t" + std::to_string(i), [](TestContext&) {});
    }

    const TestRun result = run();

    EXPECT_EQ(repository->beginRunCalls, 1) << "the run must be recorded before it starts";
    EXPECT_EQ(repository->saveResultCalls, 5) << "results are saved individually, not batched";
    EXPECT_EQ(repository->completeRunCalls, 1);

    const auto loaded = repository->loadRun(result.runId);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->results.size(), 5U);
}

TEST_F(RunnerFixture, APersistenceOutageDoesNotFailTheRun) {
    // The results are the product; the database is a convenience.
    repository = std::make_shared<InMemoryRepository>();
    repository->failNextSave = true;
    add("s", "ok", [](TestContext&) {});

    TestRun result;
    EXPECT_NO_THROW(result = run());
    EXPECT_EQ(result.results.size(), 1U);
    EXPECT_EQ(result.results[0].status, TestStatus::Passed);
}

TEST_F(RunnerFixture, DiagnosticsAreCollectedOnlyForFailures) {
    DiagnosticsConfig diagnosticsConfig;
    diagnosticsConfig.includeGpu = true;
    collector = diagnostics::DiagnosticCollector::createDefault(diagnosticsConfig);
    collector->setGpuProvider(std::make_shared<diagnostics::MockGpuProvider>(1));
    config.diagnostics.collectOnFailure = true;

    add("s", "passes", [](TestContext&) {});
    add("s", "fails", [](TestContext&) { TF_ASSERT_TRUE(false); });

    const TestRun result = run();

    // Collecting for thousands of green tests would dominate a fast suite.
    EXPECT_TRUE(find(result, "s.passes")->diagnostics.empty());
    EXPECT_FALSE(find(result, "s.fails")->diagnostics.empty());
    EXPECT_TRUE(find(result, "s.fails")->diagnostics.contains("system"));
}

TEST_F(RunnerFixture, DiagnosticsCanBeTurnedOff) {
    collector = diagnostics::DiagnosticCollector::createDefault(DiagnosticsConfig{});
    RunOptions options;
    options.collectDiagnostics = false;
    add("s", "fails", [](TestContext&) { TF_ASSERT_TRUE(false); });

    EXPECT_TRUE(run(options).results[0].diagnostics.empty());
}

// ---------------------------------------------------------------------------
// Filtering and observers
// ---------------------------------------------------------------------------

TEST_F(RunnerFixture, FiltersSelectWhatRuns) {
    add("api", "one", [](TestContext&) {}, 0, {"smoke"});
    add("api", "two", [](TestContext&) {}, 0, {"regression"});
    add("gpu", "three", [](TestContext&) {}, 0, {"hardware"});

    RunOptions bySuite;
    bySuite.filter.suites = {"api"};
    EXPECT_EQ(run(bySuite).results.size(), 2U);

    RunOptions byTag;
    byTag.filter.tags = {"smoke"};
    EXPECT_EQ(run(byTag).results.size(), 1U);

    RunOptions both;
    both.filter.suites = {"api"};
    both.filter.tags = {"regression"};
    ASSERT_EQ(run(both).results.size(), 1U);
    EXPECT_EQ(run(both).results[0].testName, "two");
}

TEST_F(RunnerFixture, AnEmptySelectionIsNotAnError) {
    add("s", "one", [](TestContext&) {});
    RunOptions options;
    options.filter.suites = {"nonexistent"};

    const TestRun result = run(options);
    EXPECT_TRUE(result.results.empty());
    EXPECT_EQ(result.exitCode(), 0);
}

namespace {

class RecordingObserver final : public RunObserver {
 public:
    void onRunStarted(const TestRun& /*run*/, const std::vector<TestMetadata>& selected) override {
        started = true;
        selectedCount = selected.size();
    }

    void onTestStarted(const TestMetadata& /*test*/) override { testsStarted.fetch_add(1); }

    void onTestFinished(const TestResult& /*result*/) override { testsFinished.fetch_add(1); }

    void onRunFinished(const TestRun& /*run*/) override { finished = true; }

    bool started = false;
    bool finished = false;
    std::size_t selectedCount = 0;
    std::atomic<int> testsStarted{0};
    std::atomic<int> testsFinished{0};
};

}  // namespace

TEST_F(RunnerFixture, ObserversSeeTheWholeLifecycle) {
    for (int i = 0; i < 4; ++i) {
        add("s", "t" + std::to_string(i), [](TestContext&) {});
    }

    auto observer = std::make_shared<RecordingObserver>();
    TestRunner runner(config, TestRegistry::instance());
    runner.addObserver(observer);
    (void)runner.run(RunOptions{});

    EXPECT_TRUE(observer->started);
    EXPECT_TRUE(observer->finished);
    EXPECT_EQ(observer->selectedCount, 4U);
    EXPECT_EQ(observer->testsStarted.load(), 4);
    EXPECT_EQ(observer->testsFinished.load(), 4);
}

TEST_F(RunnerFixture, RunOneExecutesASingleTestWithoutARun) {
    add("s", "single", [](TestContext& ctx) { ctx.addMetadata("ran", true); });

    TestRunner runner(config, TestRegistry::instance());
    const TestMetadata metadata = *TestRegistry::instance().find("s.single");
    const TestResult result = runner.runOne(metadata, "manual-run-id");

    EXPECT_EQ(result.status, TestStatus::Passed);
    EXPECT_EQ(result.runId, "manual-run-id");
    EXPECT_TRUE(result.metadata.at("ran").asBool());
}

TEST_F(RunnerFixture, RunRecordsItsEnvironment) {
    add("s", "one", [](TestContext&) {});
    const TestRun result = run();

    EXPECT_TRUE(result.environment.contains("hostname"));
    EXPECT_TRUE(result.environment.contains("workers"));
    EXPECT_TRUE(result.environment.contains("config"));
    EXPECT_FALSE(result.filterDescription.empty());
}

// ---------------------------------------------------------------------------
// Run-level cancellation (Ctrl-C, SIGTERM, fail-fast)
// ---------------------------------------------------------------------------
//
// Cancelling a run used to be indistinguishable from the tests failing: a
// cancellation-aware sleep returns early, the assertion after it does not
// hold, and the run reported FAIL. Someone who pressed Ctrl-C was told their
// code was broken. Worse, once nothing "failed" the run reported PASS with
// exit 0 — telling CI that tests it never ran were fine.

TEST_F(RunnerFixture, CancellingARunDoesNotTurnItIntoFailures) {
    // A test that cooperates the way the docs ask, and then asserts on what
    // the cooperative call returned. Under cancellation that assertion is
    // false through no fault of the test.
    for (int i = 0; i < 6; ++i) {
        add("s", "sleeper" + std::to_string(i), [](TestContext& ctx) {
            // TF_ASSERT_*, not gtest's ASSERT_*: this body runs inside
            // TestForge, and a gtest macro here would fail the enclosing
            // GoogleTest case instead of the TestForge one.
            const bool completed = ctx.sleepFor(Milliseconds{400});
            TF_ASSERT_TRUE(completed);
        });
    }

    TestRunner runner(config, TestRegistry::instance());
    std::thread canceller([&runner] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        runner.requestCancellation("interrupted by signal");
    });

    RunOptions options;
    options.workers = 1;
    const TestRun result = runner.run(options);
    canceller.join();

    EXPECT_TRUE(result.wasCancelled());
    EXPECT_EQ(result.cancellationReason, "interrupted by signal");

    const RunStatistics stats = result.statistics();
    EXPECT_EQ(stats.failed, 0) << "cancellation must never be reported as a test failure";
    EXPECT_EQ(stats.errors, 0);
    EXPECT_GT(stats.skipped, 0) << "the tests that never ran should be visible as skipped";

    for (const TestResult& one : result.results) {
        if (one.status == TestStatus::Skipped) {
            EXPECT_TRUE(one.metadata.find("run_cancelled") != nullptr)
                << one.qualifiedName() << " was skipped without saying why";
        }
    }
}

TEST_F(RunnerFixture, ACancelledRunDoesNotExitZero) {
    // The important half. Nothing failed, so the old exitCode() returned 0 and
    // CI would have gone green on a run that verified almost nothing.
    for (int i = 0; i < 6; ++i) {
        add("s", "sleeper" + std::to_string(i), [](TestContext& ctx) {
            (void)ctx.sleepFor(Milliseconds{400});
        });
    }

    TestRunner runner(config, TestRegistry::instance());
    std::thread canceller([&runner] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        runner.requestCancellation("interrupted by signal");
    });

    RunOptions options;
    options.workers = 1;
    const TestRun result = runner.run(options);
    canceller.join();

    EXPECT_EQ(result.statistics().failed, 0);
    EXPECT_NE(result.exitCode(), 0) << "an incomplete run must not look like a pass";
    EXPECT_EQ(result.exitCode(), 4);
}

TEST_F(RunnerFixture, ARealFailureOutranksCancellation) {
    // If something genuinely failed before the interrupt, that is the more
    // actionable fact and must survive in the exit code.
    add("s", "broken", [](TestContext&) { TF_ASSERT_TRUE(false); });
    for (int i = 0; i < 4; ++i) {
        add("s", "sleeper" + std::to_string(i), [](TestContext& ctx) {
            (void)ctx.sleepFor(Milliseconds{400});
        });
    }

    TestRunner runner(config, TestRegistry::instance());
    std::thread canceller([&runner] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        runner.requestCancellation("interrupted by signal");
    });

    RunOptions options;
    options.workers = 1;
    const TestRun result = runner.run(options);
    canceller.join();

    EXPECT_TRUE(result.wasCancelled());
    EXPECT_EQ(result.exitCode(), 1) << "a real failure outranks the cancellation";
}

TEST_F(RunnerFixture, AnUncancelledRunCarriesNoCancellationReason) {
    add("s", "quick", [](TestContext&) {});

    const TestRun result = run();

    EXPECT_FALSE(result.wasCancelled());
    EXPECT_TRUE(result.cancellationReason.empty());
    EXPECT_EQ(result.exitCode(), 0);
}

TEST_F(RunnerFixture, TheCancellationReasonSurvivesJson) {
    add("s", "quick", [](TestContext&) {});
    TestRun result = run();
    result.cancellationReason = "interrupted by signal";

    const TestRun restored = TestRun::fromJson(result.toJson());

    EXPECT_TRUE(restored.wasCancelled());
    EXPECT_EQ(restored.cancellationReason, "interrupted by signal");
}

// ---------------------------------------------------------------------------
// Lifetime: what an orphaned test is still allowed to touch
// ---------------------------------------------------------------------------

TEST_F(RunnerFixture, AnOrphanedTestMayOutliveTheConfigThatStartedIt) {
    // TestContext holds non-owning pointers to its Config and TestMetadata.
    // The metadata was already copied into BodyExecution so that an abandoned
    // thread could not read a freed stack frame; the Config needs the same
    // treatment for a stronger reason -- it belongs to whoever called
    // TestRunner, not to the runner, so it can go away at any time after the
    // runner does.
    //
    // The sequence below is the one that used to be a use-after-free:
    //
    //   1. the body blows its deadline and ignores cancellation,
    //   2. the runner gives up, records TIMEOUT and detaches the thread,
    //   3. the runner is destroyed, its reaper grace period expires,
    //   4. the Config is destroyed,
    //   5. only *then* does the still-running body read ctx.config().
    //
    // The Config is heap-allocated so that reading it after the free is a
    // plain heap-use-after-free, which AddressSanitizer always reports.
    auto configGone = std::make_shared<std::atomic<bool>>(false);
    auto bodyDone = std::make_shared<std::atomic<bool>>(false);
    auto observed = std::make_shared<std::string>();

    auto ownedConfig = std::make_unique<Config>();
    ownedConfig->database.enabled = false;
    ownedConfig->reporting.console = false;
    ownedConfig->reporting.json = false;
    ownedConfig->reporting.html = false;
    ownedConfig->diagnostics.collectOnFailure = false;
    ownedConfig->execution.defaultTimeoutMs = 150;
    ownedConfig->execution.cancellationGraceMs = 50;
    // A value the body can read back, to prove it saw a live copy and not
    // whatever happened to be left in the freed bytes.
    ownedConfig->custom.set("lifetime_marker", std::string("sentinel"));

    add("s", "outlives_its_config", [configGone, bodyDone, observed](TestContext& ctx) {
        // Deliberately uncooperative: this is what gets a thread detached.
        // Waiting on the flag rather than a fixed sleep keeps the ordering
        // exact instead of hoping a race lands the right way.
        for (int i = 0; i < 600 && !configGone->load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // The runner and the caller's Config are both gone by now.
        if (const json::Value* marker = ctx.config().custom.find("lifetime_marker")) {
            *observed = marker->stringOr("");
        }
        bodyDone->store(true, std::memory_order_release);
    });

    {
        TestRunner runner(*ownedConfig, TestRegistry::instance());
        RunOptions options;
        options.workers = 1;
        const TestRun run = runner.run(options);

        ASSERT_EQ(run.results.size(), 1U);
        EXPECT_EQ(run.results[0].status, TestStatus::Timeout);
        EXPECT_TRUE(run.results[0].metadata.find("abandoned_thread") != nullptr);
    }  // ~TestRunner: reaps for the grace period, then gives up on the stray

    ownedConfig.reset();  // the caller's Config is now freed
    configGone->store(true, std::memory_order_release);

    // Let the stray finish its read before this test returns, so the thread
    // cannot still be running when the next test starts.
    for (int i = 0; i < 500 && !bodyDone->load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(bodyDone->load(std::memory_order_acquire))
        << "the orphaned body never finished; the assertion below would be meaningless";
    EXPECT_EQ(*observed, "sentinel") << "the orphan read a Config that was not its own live copy";
}

// ---------------------------------------------------------------------------
// Cancellation reaches tests that have no deadline
// ---------------------------------------------------------------------------
//
// A test with no timeout runs inline on the worker thread. Nothing polls the
// run-level cancellation source on its behalf, so before requestCancellation()
// signalled active tests directly, such a test could not observe Ctrl-C at
// all: it ran to completion and the run was reported as if nothing had
// happened.

TEST_F(RunnerFixture, RunCancellationReachesATestWithNoDeadline) {
    config.execution.defaultTimeoutMs = 0;  // unbounded: executes inline

    std::atomic<bool> bodyRunning{false};
    std::atomic<bool> observedCancellation{false};
    add("s", "unbounded_cooperative", [&bodyRunning, &observedCancellation](TestContext& ctx) {
        bodyRunning.store(true, std::memory_order_release);
        // sleepFor returns false when the wait ended because of cancellation,
        // so the observation and the exit come from a single read.
        for (int i = 0; i < 600; ++i) {
            if (!ctx.sleepFor(Milliseconds{10})) {
                observedCancellation.store(true, std::memory_order_release);
                break;
            }
        }
        ctx.throwIfCancelled();
    });

    TestRunner runner(config, TestRegistry::instance());
    // Cancel once the body is actually running, rather than on a timer. A
    // fixed delay races the runner's own start-up: under a sanitizer the timer
    // can expire first, the run is cancelled before runOne() is reached, and
    // the test is skipped without its body ever executing -- which is not the
    // path this test exists to cover.
    std::thread canceller([&runner, &bodyRunning] {
        while (!bodyRunning.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        runner.requestCancellation("interrupted by signal");
    });

    RunOptions options;
    options.workers = 1;
    const Stopwatch watch;
    const TestRun run = runner.run(options);
    const Milliseconds elapsed = watch.elapsed();
    canceller.join();

    EXPECT_TRUE(observedCancellation.load()) << "an unbounded test never saw the cancellation";
    EXPECT_LT(elapsed.count(), 3000) << "the test ran to completion instead of stopping when asked";

    ASSERT_EQ(run.results.size(), 1U);
    EXPECT_EQ(run.results[0].status, TestStatus::Skipped)
        << "a test stopped by a run cancellation is not a failure";
    EXPECT_EQ(run.results[0].failureCategory, FailureCategory::None);
    EXPECT_TRUE(run.results[0].metadata.find("run_cancelled") != nullptr);
    EXPECT_TRUE(run.wasCancelled());
    EXPECT_EQ(run.exitCode(), 4);
}

TEST_F(RunnerFixture, AnUnboundedTestThatIgnoresCancellationStillCompletes) {
    // The other half of the contract. TestForge never kills a thread, so an
    // unbounded test that does not check its token runs to the end -- but the
    // run must still finish, and must not hang waiting for it.
    config.execution.defaultTimeoutMs = 0;

    add("s", "unbounded_uncooperative", [](TestContext&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });

    TestRunner runner(config, TestRegistry::instance());
    std::thread canceller([&runner] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        runner.requestCancellation("interrupted by signal");
    });

    RunOptions options;
    options.workers = 1;
    const TestRun run = runner.run(options);
    canceller.join();

    ASSERT_EQ(run.results.size(), 1U);
    // It finished on its own terms, but the run was cancelled underneath it,
    // so its verdict is discarded rather than reported as a pass.
    EXPECT_EQ(run.results[0].status, TestStatus::Skipped);
    EXPECT_TRUE(run.wasCancelled());
}

TEST_F(RunnerFixture, AnUnboundedTestIsUnaffectedWhenNothingCancels) {
    // Guard against the fix over-firing: with no cancellation an unbounded
    // test must still be reported on its own merits.
    config.execution.defaultTimeoutMs = 0;

    add("s", "unbounded_passes", [](TestContext& ctx) {
        ctx.throwIfCancelled();
        (void)ctx.sleepFor(Milliseconds{10});
    });
    add("s", "unbounded_fails", [](TestContext&) { TF_ASSERT_TRUE(false); });

    const TestRun result = run();

    const TestResult* passed = find(result, "s.unbounded_passes");
    const TestResult* failed = find(result, "s.unbounded_fails");
    ASSERT_NE(passed, nullptr);
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(passed->status, TestStatus::Passed);
    EXPECT_EQ(failed->status, TestStatus::Failed);
    EXPECT_FALSE(result.wasCancelled());
    EXPECT_EQ(result.exitCode(), 1);
}
