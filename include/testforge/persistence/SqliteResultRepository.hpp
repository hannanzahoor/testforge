#pragma once

#include "testforge/persistence/Database.hpp"
#include "testforge/persistence/ResultRepository.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace testforge {

/// SQLite-backed implementation of ResultRepository.
///
/// SQLite because a test tool should not require a server to be running before
/// it can record a result: the database is a file, it travels with the
/// workspace, and CI can upload it as an artefact. The interface is what the
/// engine depends on, so swapping in PostgreSQL later is a new class rather
/// than a refactor (docs/database.md).
///
/// Thread safety: one connection guarded by a mutex. Results arrive from
/// several worker threads, but a write takes microseconds while a test takes
/// milliseconds, so the lock is never the bottleneck — and it keeps the
/// transaction semantics obvious.
class SqliteResultRepository final : public ResultRepository {
 public:
    /// Opens (or creates) the database at `path`. ":memory:" is supported and
    /// is what the unit tests use.
    explicit SqliteResultRepository(const std::string& path, std::int64_t busyTimeoutMs = 5000);

    ~SqliteResultRepository() override;

    void initialise() override;

    void beginRun(const TestRun& run) override;

    void saveResult(const TestResult& result) override;

    void completeRun(const TestRun& run) override;

    void recordEvent(const std::string& runId,
                     const std::string& type,
                     const json::Value& payload) override;

    [[nodiscard]] std::optional<TestRun> loadRun(const std::string& runId) override;

    [[nodiscard]] std::vector<TestResult> loadResults(const std::string& runId) override;

    [[nodiscard]] std::vector<TestRun> listRuns(const RunQuery& query) override;

    [[nodiscard]] std::vector<TestHistoryEntry> testHistory(const std::string& testId,
                                                            int limit) override;

    [[nodiscard]] std::vector<TestHistorySummary> historySummaries(int limit) override;

    [[nodiscard]] json::Value aggregateStatistics(int runLimit) override;

    int pruneOlderThan(int days) override;

    [[nodiscard]] json::Value storageInfo() override;

    /// Current schema version, for migration tests.
    [[nodiscard]] int schemaVersion();

 private:
    /// Applies any migrations the file has not seen yet.
    void migrate();

    mutable std::mutex mutex_;
    std::unique_ptr<db::Connection> connection_;
    std::string path_;
};

}  // namespace testforge
