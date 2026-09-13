#pragma once

#include "testforge/core/Json.hpp"
#include "testforge/core/TestResult.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace testforge {

/// One row of per-test history.
struct TestHistoryEntry {
    std::string runId;
    TestStatus status = TestStatus::Error;
    FailureCategory failureCategory = FailureCategory::None;
    TimePoint startTime;
    Milliseconds duration{0};
    std::string errorMessage;

    [[nodiscard]] json::Value toJson() const;
};

/// Aggregate history for one test, used for flake detection and trends.
struct TestHistorySummary {
    std::string testId;
    std::string qualifiedName;
    int totalRuns = 0;
    int passes = 0;
    int failures = 0;
    int skips = 0;
    Milliseconds averageDuration{0};
    Milliseconds p95Duration{0};
    TimePoint lastRun;
    TestStatus lastStatus = TestStatus::Error;

    /// How often the outcome changed between consecutive runs, as a fraction
    /// of the transitions observed.
    ///
    /// HEURISTIC. A high score means the test alternated pass/fail without
    /// anyone changing it, which is *suggestive* of flakiness — but the same
    /// pattern appears when a real bug is intermittent, or when a dependency
    /// was down for an hour. TestForge flags candidates; it does not diagnose.
    double flipRate = 0.0;

    [[nodiscard]] bool looksFlaky(int minimumRuns = 5, double threshold = 0.2) const;

    [[nodiscard]] json::Value toJson() const;
};

/// Query filter for run listings.
struct RunQuery {
    std::optional<std::string> labelContains;
    std::optional<TimePoint> since;
    std::optional<TimePoint> until;
    int limit = 50;
    int offset = 0;
};

/// Persistence boundary.
///
/// The execution engine depends on this interface and never on SQLite. That
/// keeps the engine testable with an in-memory fake (see tests/), and leaves
/// room for a PostgreSQL implementation without touching a line of runner
/// code — see docs/database.md.
///
/// Implementations must be safe to call from multiple threads.
class ResultRepository {
 public:
    virtual ~ResultRepository() = default;

    ResultRepository(const ResultRepository&) = delete;
    ResultRepository& operator=(const ResultRepository&) = delete;
    ResultRepository(ResultRepository&&) = delete;
    ResultRepository& operator=(ResultRepository&&) = delete;

    /// Creates the schema if it does not exist. Idempotent.
    virtual void initialise() = 0;

    /// Records the start of a run. Called before any results arrive so a
    /// crashed run still leaves a trace.
    virtual void beginRun(const TestRun& run) = 0;

    /// Stores one result. Called as each test finishes, not in a batch at the
    /// end — an interrupted run keeps the results it already produced.
    virtual void saveResult(const TestResult& result) = 0;

    /// Finalises a run: end time, duration, aggregate counts.
    virtual void completeRun(const TestRun& run) = 0;

    /// Records a free-form event against a run (AI analysis, diagnostics
    /// collection, retries).
    virtual void recordEvent(const std::string& runId,
                             const std::string& type,
                             const json::Value& payload) = 0;

    [[nodiscard]] virtual std::optional<TestRun> loadRun(const std::string& runId) = 0;

    [[nodiscard]] virtual std::vector<TestResult> loadResults(const std::string& runId) = 0;

    [[nodiscard]] virtual std::vector<TestRun> listRuns(const RunQuery& query) = 0;

    [[nodiscard]] virtual std::vector<TestHistoryEntry> testHistory(const std::string& testId,
                                                                    int limit) = 0;

    /// History summaries across every known test, newest activity first.
    [[nodiscard]] virtual std::vector<TestHistorySummary> historySummaries(int limit) = 0;

    /// Aggregate statistics over the last `runLimit` runs.
    [[nodiscard]] virtual json::Value aggregateStatistics(int runLimit) = 0;

    /// Deletes runs older than `days`. Returns how many were removed.
    virtual int pruneOlderThan(int days) = 0;

    /// Total rows, for `testforge stats`.
    [[nodiscard]] virtual json::Value storageInfo() = 0;

 protected:
    ResultRepository() = default;
};

using ResultRepositoryPtr = std::shared_ptr<ResultRepository>;

}  // namespace testforge
