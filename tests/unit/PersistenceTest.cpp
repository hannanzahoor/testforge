/// Tests for the SQLite layer: RAII wrappers, schema migration, and the
/// repository. Everything runs against ":memory:", so there is no temporary
/// file to clean up and no shared state between tests.

#include "testforge/core/Exceptions.hpp"
#include "testforge/core/Ids.hpp"
#include "testforge/persistence/Database.hpp"
#include "testforge/persistence/SqliteResultRepository.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace testforge;

// ---------------------------------------------------------------------------
// Low-level wrappers
// ---------------------------------------------------------------------------

TEST(Database, OpensInMemory) {
    db::Connection connection(":memory:");
    EXPECT_NE(connection.handle(), nullptr);
    EXPECT_FALSE(db::Connection::libraryVersion().empty());
    EXPECT_EQ(connection.integrityCheck(), "ok");
}

TEST(Database, RejectsAnUnopenablePath) {
    EXPECT_THROW(db::Connection("/nonexistent-directory/testforge.db"), PersistenceError);
}

TEST(Database, ParametersAreBoundNotInterpolated) {
    db::Connection connection(":memory:");
    connection.executeScript("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT);");

    // The classic injection payload, and an apostrophe that would break naive
    // string concatenation. Both must survive as literal data.
    const std::string hostile = "Robert'); DROP TABLE t;--";
    {
        db::Statement insert(connection, "INSERT INTO t (name) VALUES (?);");
        insert.bind(1, hostile);
        insert.execute();
    }

    db::Statement query(connection, "SELECT name FROM t;");
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.getText(0), hostile);
    EXPECT_FALSE(query.step());

    // The table is still there, which it would not be if the payload had run.
    db::Statement count(connection, "SELECT COUNT(*) FROM t;");
    ASSERT_TRUE(count.step());
    EXPECT_EQ(count.getInt(0), 1);
}

TEST(Database, BindsEveryScalarType) {
    db::Connection connection(":memory:");
    connection.executeScript("CREATE TABLE t (i INTEGER, d REAL, s TEXT, b INTEGER, n TEXT);");

    db::Statement insert(connection, "INSERT INTO t VALUES (?, ?, ?, ?, ?);");
    insert.bindAll(std::int64_t{42}, 3.5, std::string("text"), true, nullptr);
    insert.execute();

    db::Statement query(connection, "SELECT i, d, s, b, n FROM t;");
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.getInt(0), 42);
    EXPECT_DOUBLE_EQ(query.getDouble(1), 3.5);
    EXPECT_EQ(query.getText(2), "text");
    EXPECT_TRUE(query.getBool(3));
    EXPECT_TRUE(query.isNull(4));
}

TEST(Database, BoundStringsAreCopied) {
    // SQLITE_TRANSIENT: the caller's buffer must not have to outlive the
    // statement. Getting this wrong is the classic source of garbage rows.
    db::Connection connection(":memory:");
    connection.executeScript("CREATE TABLE t (s TEXT);");

    db::Statement insert(connection, "INSERT INTO t VALUES (?);");
    {
        const std::string temporary = "this string goes out of scope";
        insert.bind(1, temporary);
    }
    insert.execute();

    db::Statement query(connection, "SELECT s FROM t;");
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.getText(0), "this string goes out of scope");
}

TEST(Database, PreparingBadSqlThrows) {
    db::Connection connection(":memory:");
    EXPECT_THROW(db::Statement(connection, "SELCT nonsense"), PersistenceError);
}

TEST(Database, ExecuteRefusesAStatementThatReturnsRows) {
    // Silently discarding rows would hide a mistake in the caller.
    db::Connection connection(":memory:");
    connection.executeScript("CREATE TABLE t (i INTEGER); INSERT INTO t VALUES (1);");
    db::Statement query(connection, "SELECT i FROM t;");
    EXPECT_THROW(query.execute(), PersistenceError);
}

TEST(Database, TransactionCommits) {
    db::Connection connection(":memory:");
    connection.executeScript("CREATE TABLE t (i INTEGER);");
    {
        db::Transaction transaction(connection);
        db::Statement insert(connection, "INSERT INTO t VALUES (1);");
        insert.execute();
        transaction.commit();
    }
    db::Statement count(connection, "SELECT COUNT(*) FROM t;");
    ASSERT_TRUE(count.step());
    EXPECT_EQ(count.getInt(0), 1);
}

TEST(Database, TransactionRollsBackWhenNotCommitted) {
    db::Connection connection(":memory:");
    connection.executeScript("CREATE TABLE t (i INTEGER);");
    {
        db::Transaction transaction(connection);
        db::Statement insert(connection, "INSERT INTO t VALUES (1);");
        insert.execute();
        // No commit: the destructor rolls back.
    }
    db::Statement count(connection, "SELECT COUNT(*) FROM t;");
    ASSERT_TRUE(count.step());
    EXPECT_EQ(count.getInt(0), 0);
}

TEST(Database, StatementsAreMovable) {
    db::Connection connection(":memory:");
    connection.executeScript("CREATE TABLE t (i INTEGER); INSERT INTO t VALUES (7);");

    db::Statement first(connection, "SELECT i FROM t;");
    db::Statement second = std::move(first);
    ASSERT_TRUE(second.step());
    EXPECT_EQ(second.getInt(0), 7);
}

// ---------------------------------------------------------------------------
// Repository
// ---------------------------------------------------------------------------

namespace {

TestResult makeResult(const std::string& runId,
                      const std::string& suite,
                      const std::string& name,
                      TestStatus status,
                      FailureCategory category = FailureCategory::None) {
    TestResult result = TestResult::make(suite, name, status);
    result.runId = runId;
    result.failureCategory = category;
    result.duration = Milliseconds{25};
    result.startTime = WallClock::now();
    result.endTime = result.startTime;
    result.tags = {suite, "unit"};
    result.metadata.set("http_status", 200);
    return result;
}

TestRun makeRun(const std::string& runId) {
    TestRun run;
    run.runId = runId;
    run.label = "unit";
    run.startedAt = WallClock::now();
    run.finishedAt = run.startedAt;
    run.duration = Milliseconds{100};
    run.workers = 2;
    run.filterDescription = "--suite unit";
    run.hostname = "test-host";
    run.gitCommit = "abc1234";
    run.environment.set("key", "value");
    return run;
}

std::unique_ptr<SqliteResultRepository> freshRepository() {
    auto repository = std::make_unique<SqliteResultRepository>(":memory:");
    repository->initialise();
    return repository;
}

}  // namespace

TEST(Repository, MigrationIsIdempotent) {
    auto repository = freshRepository();
    const int version = repository->schemaVersion();
    EXPECT_GT(version, 0);

    // Running initialise again must not re-apply anything.
    repository->initialise();
    EXPECT_EQ(repository->schemaVersion(), version);
}

TEST(Repository, RoundTripsARun) {
    auto repository = freshRepository();
    TestRun run = makeRun("run-1");
    repository->beginRun(run);

    const TestResult passed = makeResult("run-1", "api", "health", TestStatus::Passed);
    const TestResult failed =
        makeResult("run-1", "api", "broken", TestStatus::Failed, FailureCategory::AssertionFailure);
    repository->saveResult(passed);
    repository->saveResult(failed);

    run.results = {passed, failed};
    repository->completeRun(run);

    const auto loaded = repository->loadRun("run-1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->runId, "run-1");
    EXPECT_EQ(loaded->hostname, "test-host");
    EXPECT_EQ(loaded->gitCommit, "abc1234");
    EXPECT_EQ(loaded->workers, 2);
    ASSERT_EQ(loaded->results.size(), 2U);

    const RunStatistics stats = loaded->statistics();
    EXPECT_EQ(stats.passed, 1);
    EXPECT_EQ(stats.failed, 1);
}

TEST(Repository, ResultFieldsSurviveTheRoundTrip) {
    auto repository = freshRepository();
    repository->beginRun(makeRun("run-2"));

    TestResult result =
        makeResult("run-2", "api", "detail", TestStatus::Failed, FailureCategory::NetworkFailure);
    result.errorMessage = "connection refused";
    result.errorDetail = "multi\nline\ndetail";
    result.logs = {"line one", "line two"};
    result.attempt = 3;
    result.flakyCandidate = true;
    result.worker = "worker-2";
    result.diagnostics.set("system", json::Value::object());
    repository->saveResult(result);

    const std::vector<TestResult> loaded = repository->loadResults("run-2");
    ASSERT_EQ(loaded.size(), 1U);
    EXPECT_EQ(loaded[0].errorMessage, "connection refused");
    EXPECT_EQ(loaded[0].errorDetail, "multi\nline\ndetail");
    EXPECT_EQ(loaded[0].logs.size(), 2U);
    EXPECT_EQ(loaded[0].tags.size(), 2U);
    EXPECT_EQ(loaded[0].attempt, 3);
    EXPECT_TRUE(loaded[0].flakyCandidate);
    EXPECT_EQ(loaded[0].worker, "worker-2");
    EXPECT_EQ(loaded[0].failureCategory, FailureCategory::NetworkFailure);
    EXPECT_EQ(loaded[0].metadata.at("http_status").asInt(), 200);
}

TEST(Repository, UnknownRunIsNullopt) {
    auto repository = freshRepository();
    EXPECT_FALSE(repository->loadRun("no-such-run").has_value());
    EXPECT_TRUE(repository->loadResults("no-such-run").empty());
}

TEST(Repository, ListsRunsNewestFirst) {
    auto repository = freshRepository();
    for (int i = 1; i <= 5; ++i) {
        TestRun run = makeRun("run-" + std::to_string(i));
        run.startedAt = WallClock::now() + std::chrono::seconds(i);
        repository->beginRun(run);
        repository->completeRun(run);
    }

    RunQuery query;
    query.limit = 3;
    const std::vector<TestRun> runs = repository->listRuns(query);
    ASSERT_EQ(runs.size(), 3U);
    EXPECT_EQ(runs[0].runId, "run-5");
    EXPECT_EQ(runs[2].runId, "run-3");
}

TEST(Repository, ListingCarriesCountsWithoutLoadingEveryResult) {
    auto repository = freshRepository();
    TestRun run = makeRun("run-counts");
    repository->beginRun(run);
    for (int i = 0; i < 7; ++i) {
        repository->saveResult(
            makeResult("run-counts", "api", "t" + std::to_string(i), TestStatus::Passed));
    }
    repository->saveResult(makeResult(
        "run-counts", "api", "bad", TestStatus::Failed, FailureCategory::AssertionFailure));
    run.results.assign(7, makeResult("run-counts", "api", "x", TestStatus::Passed));
    run.results.push_back(makeResult(
        "run-counts", "api", "bad", TestStatus::Failed, FailureCategory::AssertionFailure));
    repository->completeRun(run);

    const std::vector<TestRun> runs = repository->listRuns(RunQuery{});
    ASSERT_EQ(runs.size(), 1U);
    const RunStatistics stats = runs[0].statistics();
    EXPECT_EQ(stats.total, 8);
    EXPECT_EQ(stats.passed, 7);
    EXPECT_EQ(stats.failed, 1);
}

TEST(Repository, TestHistoryIsPerTest) {
    auto repository = freshRepository();
    const std::string testId = ids::testId("api", "flaky");

    for (int i = 0; i < 6; ++i) {
        const std::string runId = "run-" + std::to_string(i);
        repository->beginRun(makeRun(runId));
        repository->saveResult(
            makeResult(runId,
                       "api",
                       "flaky",
                       i % 2 == 0 ? TestStatus::Passed : TestStatus::Failed,
                       i % 2 == 0 ? FailureCategory::None : FailureCategory::AssertionFailure));
        repository->saveResult(makeResult(runId, "api", "stable", TestStatus::Passed));
    }

    const std::vector<TestHistoryEntry> history = repository->testHistory(testId, 10);
    EXPECT_EQ(history.size(), 6U);
}

TEST(Repository, FlakeHeuristicNeedsAlternationAndEnoughRuns) {
    auto repository = freshRepository();

    for (int i = 0; i < 8; ++i) {
        const std::string runId = "run-" + std::to_string(i);
        TestRun run = makeRun(runId);
        run.startedAt = WallClock::now() + std::chrono::seconds(i);
        repository->beginRun(run);

        TestResult alternating =
            makeResult(runId,
                       "api",
                       "alternating",
                       i % 2 == 0 ? TestStatus::Passed : TestStatus::Failed,
                       i % 2 == 0 ? FailureCategory::None : FailureCategory::AssertionFailure);
        alternating.startTime = run.startedAt;
        repository->saveResult(alternating);

        TestResult stable = makeResult(runId, "api", "stable", TestStatus::Passed);
        stable.startTime = run.startedAt;
        repository->saveResult(stable);
        repository->completeRun(run);
    }

    const std::vector<TestHistorySummary> summaries = repository->historySummaries(50);
    ASSERT_FALSE(summaries.empty());

    for (const TestHistorySummary& summary : summaries) {
        if (summary.qualifiedName == "api.alternating") {
            EXPECT_GT(summary.flipRate, 0.5);
            EXPECT_TRUE(summary.looksFlaky());
        } else if (summary.qualifiedName == "api.stable") {
            EXPECT_DOUBLE_EQ(summary.flipRate, 0.0);
            EXPECT_FALSE(summary.looksFlaky()) << "a test that always passes is not flaky";
        }
    }
}

TEST(Repository, FlakeHeuristicIgnoresTooFewRuns) {
    // One pass and one fail gives a flip rate of 1.0, which would be reported
    // as maximally flaky without the minimum-runs guard.
    TestHistorySummary summary;
    summary.totalRuns = 2;
    summary.passes = 1;
    summary.failures = 1;
    summary.flipRate = 1.0;
    EXPECT_FALSE(summary.looksFlaky());

    summary.totalRuns = 10;
    EXPECT_TRUE(summary.looksFlaky());
}

TEST(Repository, AggregateStatistics) {
    auto repository = freshRepository();
    TestRun run = makeRun("run-stats");
    repository->beginRun(run);

    repository->saveResult(makeResult("run-stats", "api", "a", TestStatus::Passed));
    repository->saveResult(
        makeResult("run-stats", "api", "b", TestStatus::Failed, FailureCategory::AssertionFailure));
    repository->saveResult(
        makeResult("run-stats", "api", "c", TestStatus::Failed, FailureCategory::NetworkFailure));
    run.results = repository->loadResults("run-stats");
    repository->completeRun(run);

    const json::Value stats = repository->aggregateStatistics(10);
    EXPECT_EQ(stats.at("runs").asInt(), 1);
    EXPECT_EQ(stats.at("total_tests").asInt(), 3);
    EXPECT_EQ(stats.at("passed").asInt(), 1);
    EXPECT_EQ(stats.at("failed").asInt(), 2);

    ASSERT_TRUE(stats.at("failure_categories").isArray());
    EXPECT_EQ(stats.at("failure_categories").size(), 2U);
    EXPECT_FALSE(stats.at("top_failing_tests").asArray().empty());
}

TEST(Repository, EventsAreRecorded) {
    auto repository = freshRepository();
    repository->beginRun(makeRun("run-events"));

    json::Value payload = json::Value::object();
    payload.set("note", "diagnostics collected");
    repository->recordEvent("run-events", "diagnostics", payload);

    const json::Value info = repository->storageInfo();
    EXPECT_EQ(info.at("event_count").asInt(), 1);
}

TEST(Repository, PruneRemovesOldRunsAndTheirResults) {
    auto repository = freshRepository();

    TestRun old = makeRun("run-old");
    old.startedAt = WallClock::now() - std::chrono::hours(24 * 100);
    repository->beginRun(old);
    repository->saveResult(makeResult("run-old", "api", "a", TestStatus::Passed));

    TestRun recent = makeRun("run-recent");
    repository->beginRun(recent);
    repository->saveResult(makeResult("run-recent", "api", "a", TestStatus::Passed));

    EXPECT_EQ(repository->pruneOlderThan(30), 1);
    EXPECT_FALSE(repository->loadRun("run-old").has_value());
    EXPECT_TRUE(repository->loadRun("run-recent").has_value());
    // ON DELETE CASCADE must have taken the results with it.
    EXPECT_TRUE(repository->loadResults("run-old").empty());
}

TEST(Repository, PruneWithZeroDaysIsANoOp) {
    auto repository = freshRepository();
    repository->beginRun(makeRun("run-keep"));
    EXPECT_EQ(repository->pruneOlderThan(0), 0);
    EXPECT_TRUE(repository->loadRun("run-keep").has_value());
}

TEST(Repository, StorageInfoDescribesTheDatabase) {
    auto repository = freshRepository();
    const json::Value info = repository->storageInfo();
    EXPECT_EQ(info.at("path").asString(), ":memory:");
    EXPECT_FALSE(info.at("sqlite_version").asString().empty());
    EXPECT_EQ(info.at("run_count").asInt(), 0);
}

TEST(Repository, ConcurrentWritesAreSerialised) {
    // Results arrive from several worker threads; the repository must not
    // corrupt or lose any of them.
    auto repository = freshRepository();
    repository->beginRun(makeRun("run-concurrent"));

    constexpr int kThreads = 4;
    constexpr int kPerThread = 25;

    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&repository, t] {
            for (int i = 0; i < kPerThread; ++i) {
                repository->saveResult(makeResult("run-concurrent",
                                                  "api",
                                                  "t" + std::to_string(t) + "_" + std::to_string(i),
                                                  TestStatus::Passed));
            }
        });
    }
    for (std::thread& writer : writers) {
        writer.join();
    }

    EXPECT_EQ(repository->loadResults("run-concurrent").size(),
              static_cast<std::size_t>(kThreads * kPerThread));
}
