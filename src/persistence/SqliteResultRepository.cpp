#include "testforge/persistence/SqliteResultRepository.hpp"

#include "testforge/core/Ids.hpp"
#include "testforge/core/Logger.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/persistence/Schema.hpp"

#include <algorithm>
#include <map>
#include <sstream>

namespace testforge {
namespace {

json::Value parseJsonColumn(const std::string& text, json::Value fallback) {
    if (text.empty()) {
        return fallback;
    }
    std::optional<json::Value> parsed = json::tryParse(text);
    return parsed.has_value() ? *parsed : std::move(fallback);
}

std::vector<std::string> parseStringArray(const std::string& text) {
    std::vector<std::string> out;
    const json::Value value = parseJsonColumn(text, json::Value::array());
    if (!value.isArray()) {
        return out;
    }
    for (const json::Value& item : value.asArray()) {
        if (item.isString()) {
            out.push_back(item.asString());
        }
    }
    return out;
}

std::string dumpStringArray(const std::vector<std::string>& items) {
    json::Value array = json::Value::array();
    for (const std::string& item : items) {
        array.push(item);
    }
    return array.dump();
}

TestResult readResultRow(db::Statement& row) {
    TestResult result;
    result.runId = row.getText(0);
    result.testId = row.getText(1);
    result.testName = row.getText(2);
    result.suiteName = row.getText(3);
    if (const std::optional<TestStatus> status = testStatusFromString(row.getText(4));
        status.has_value()) {
        result.status = *status;
    }
    if (const std::optional<FailureCategory> category = failureCategoryFromString(row.getText(5));
        category.has_value()) {
        result.failureCategory = *category;
    }
    result.startTime = fromEpochMillis(row.getInt(6));
    result.endTime = fromEpochMillis(row.getInt(7));
    result.duration = Milliseconds{row.getInt(8)};
    result.errorMessage = row.getText(9);
    result.errorDetail = row.getText(10);
    result.logs = parseStringArray(row.getText(11));
    result.tags = parseStringArray(row.getText(12));
    result.metadata = parseJsonColumn(row.getText(13), json::Value::object());
    result.diagnostics = parseJsonColumn(row.getText(14), json::Value::object());
    result.attempt = static_cast<int>(row.getInt(15));
    result.flakyCandidate = row.getBool(16);
    result.worker = row.getText(17);
    return result;
}

constexpr std::string_view kResultColumns =
    "run_id, test_id, test_name, suite_name, status, failure_category, start_time_ms, "
    "end_time_ms, duration_ms, error_message, error_detail, logs_json, tags_json, "
    "metadata_json, diagnostics_json, attempt, flaky_candidate, worker";

TestRun readRunRow(db::Statement& row) {
    TestRun run;
    run.runId = row.getText(0);
    run.label = row.getText(1);
    run.startedAt = fromEpochMillis(row.getInt(2));
    run.finishedAt = fromEpochMillis(row.getInt(3));
    run.duration = Milliseconds{row.getInt(4)};
    run.workers = static_cast<int>(row.getInt(5));
    run.filterDescription = row.getText(6);
    run.gitCommit = row.getText(7);
    run.hostname = row.getText(8);
    run.environment = parseJsonColumn(row.getText(9), json::Value::object());
    return run;
}

constexpr std::string_view kRunColumns =
    "run_id, label, started_at_ms, finished_at_ms, duration_ms, workers, filter, git_commit, "
    "hostname, environment_json";

/// Caps the size of a text column so one pathological result cannot bloat the
/// database. Diagnostics and log tails are the usual offenders.
std::string capped(const std::string& text, std::size_t limit) {
    return text.size() <= limit ? text : strings::truncate(text, limit);
}

}  // namespace

SqliteResultRepository::SqliteResultRepository(const std::string& path, std::int64_t busyTimeoutMs)
    : connection_(std::make_unique<db::Connection>(path, busyTimeoutMs)), path_(path) {}

SqliteResultRepository::~SqliteResultRepository() = default;

void SqliteResultRepository::initialise() {
    const std::lock_guard<std::mutex> lock(mutex_);
    migrate();
}

void SqliteResultRepository::migrate() {
    connection_->executeScript(
        "CREATE TABLE IF NOT EXISTS schema_version ("
        "  version INTEGER PRIMARY KEY,"
        "  applied_ms INTEGER NOT NULL,"
        "  description TEXT NOT NULL DEFAULT ''"
        ");");

    int applied = 0;
    {
        db::Statement query(*connection_, "SELECT COALESCE(MAX(version), 0) FROM schema_version;");
        if (query.step()) {
            applied = static_cast<int>(query.getInt(0));
        }
    }

    for (const schema::Migration& migration : schema::migrations()) {
        if (migration.version <= applied) {
            continue;
        }
        db::Transaction transaction(*connection_);
        connection_->executeScript(migration.sql);
        db::Statement record(
            *connection_,
            "INSERT INTO schema_version (version, applied_ms, description) VALUES (?, ?, ?);");
        record.bindAll(static_cast<std::int64_t>(migration.version),
                       toEpochMillis(WallClock::now()),
                       migration.description);
        record.execute();
        transaction.commit();

        Logger("persistence")
            .info("applied schema migration",
                  {{"version", migration.version}, {"description", migration.description}});
    }
}

int SqliteResultRepository::schemaVersion() {
    const std::lock_guard<std::mutex> lock(mutex_);
    db::Statement query(*connection_, "SELECT COALESCE(MAX(version), 0) FROM schema_version;");
    return query.step() ? static_cast<int>(query.getInt(0)) : 0;
}

void SqliteResultRepository::beginRun(const TestRun& run) {
    const std::lock_guard<std::mutex> lock(mutex_);
    db::Statement insert(*connection_,
                         "INSERT OR REPLACE INTO test_runs "
                         "(run_id, label, started_at_ms, duration_ms, workers, filter, "
                         " git_commit, hostname, environment_json, completed) "
                         "VALUES (?, ?, ?, 0, ?, ?, ?, ?, ?, 0);");
    insert.bindAll(run.runId,
                   run.label,
                   toEpochMillis(run.startedAt),
                   run.workers,
                   run.filterDescription,
                   run.gitCommit,
                   run.hostname,
                   run.environment.dump());
    insert.execute();
}

void SqliteResultRepository::saveResult(const TestResult& result) {
    const std::lock_guard<std::mutex> lock(mutex_);
    db::Statement insert(
        *connection_,
        "INSERT INTO test_results "
        "(run_id, test_id, test_name, suite_name, qualified_name, status, failure_category, "
        " start_time_ms, end_time_ms, duration_ms, error_message, error_detail, logs_json, "
        " tags_json, metadata_json, diagnostics_json, attempt, flaky_candidate, worker) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");

    insert.bindAll(result.runId,
                   result.testId,
                   result.testName,
                   result.suiteName,
                   result.qualifiedName(),
                   std::string(toString(result.status)),
                   std::string(toString(result.failureCategory)),
                   toEpochMillis(result.startTime),
                   toEpochMillis(result.endTime),
                   millisOf(result.duration),
                   capped(result.errorMessage, 4000),
                   capped(result.errorDetail, 16000),
                   capped(dumpStringArray(result.logs), 64000),
                   dumpStringArray(result.tags),
                   capped(result.metadata.dump(), 64000),
                   capped(result.diagnostics.dump(), 128000),
                   result.attempt,
                   result.flakyCandidate,
                   result.worker);
    insert.execute();
}

void SqliteResultRepository::completeRun(const TestRun& run) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const RunStatistics stats = run.statistics();

    db::Statement update(*connection_,
                         "UPDATE test_runs SET finished_at_ms = ?, duration_ms = ?, "
                         " total = ?, passed = ?, failed = ?, skipped = ?, errors = ?, "
                         " timeouts = ?, completed = 1 "
                         "WHERE run_id = ?;");
    update.bindAll(toEpochMillis(run.finishedAt),
                   millisOf(run.duration),
                   stats.total,
                   stats.passed,
                   stats.failed,
                   stats.skipped,
                   stats.errors,
                   stats.timeouts,
                   run.runId);
    update.execute();
}

void SqliteResultRepository::recordEvent(const std::string& runId,
                                         const std::string& type,
                                         const json::Value& payload) {
    const std::lock_guard<std::mutex> lock(mutex_);
    db::Statement insert(*connection_,
                         "INSERT INTO test_events (run_id, created_ms, event_type, payload_json) "
                         "VALUES (?, ?, ?, ?);");
    insert.bindAll(runId, toEpochMillis(WallClock::now()), type, capped(payload.dump(), 128000));
    insert.execute();
}

std::optional<TestRun> SqliteResultRepository::loadRun(const std::string& runId) {
    std::optional<TestRun> run;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        db::Statement query(
            *connection_,
            "SELECT " + std::string(kRunColumns) + " FROM test_runs WHERE run_id = ?;");
        query.bind(1, runId);
        if (!query.step()) {
            return std::nullopt;
        }
        run = readRunRow(query);
    }
    // loadResults takes the lock itself, so it must be called after releasing.
    run->results = loadResults(runId);
    return run;
}

std::vector<TestResult> SqliteResultRepository::loadResults(const std::string& runId) {
    const std::lock_guard<std::mutex> lock(mutex_);
    db::Statement query(*connection_,
                        "SELECT " + std::string(kResultColumns) +
                            " FROM test_results WHERE run_id = ? "
                            "ORDER BY qualified_name, attempt;");
    query.bind(1, runId);

    std::vector<TestResult> results;
    while (query.step()) {
        results.push_back(readResultRow(query));
    }
    return results;
}

std::vector<TestRun> SqliteResultRepository::listRuns(const RunQuery& query) {
    const std::lock_guard<std::mutex> lock(mutex_);

    // The WHERE clause is assembled from a fixed set of fragments; every value
    // is still bound, never interpolated.
    std::string sql = "SELECT " + std::string(kRunColumns) +
                      ", total, passed, failed, skipped, errors, timeouts "
                      "FROM test_runs WHERE 1 = 1";
    if (query.labelContains.has_value()) {
        sql += " AND label LIKE ?";
    }
    if (query.since.has_value()) {
        sql += " AND started_at_ms >= ?";
    }
    if (query.until.has_value()) {
        sql += " AND started_at_ms <= ?";
    }
    sql += " ORDER BY started_at_ms DESC LIMIT ? OFFSET ?;";

    db::Statement statement(*connection_, sql);
    int index = 0;
    if (query.labelContains.has_value()) {
        statement.bind(++index, "%" + *query.labelContains + "%");
    }
    if (query.since.has_value()) {
        statement.bind(++index, toEpochMillis(*query.since));
    }
    if (query.until.has_value()) {
        statement.bind(++index, toEpochMillis(*query.until));
    }
    statement.bind(++index, std::max(1, query.limit));
    statement.bind(++index, std::max(0, query.offset));

    std::vector<TestRun> runs;
    while (statement.step()) {
        TestRun run = readRunRow(statement);
        // Summaries carry the stored counts rather than re-reading every
        // result row: a listing of 50 runs must not load 50 000 results.
        // Synthesising placeholder results keeps TestRun::statistics() correct
        // for callers that only ever look at the aggregate.
        const int total = static_cast<int>(statement.getInt(10));
        const int passed = static_cast<int>(statement.getInt(11));
        const int failed = static_cast<int>(statement.getInt(12));
        const int skipped = static_cast<int>(statement.getInt(13));
        const int errors = static_cast<int>(statement.getInt(14));
        const int timeouts = static_cast<int>(statement.getInt(15));

        const auto append = [&run](TestStatus status, int count) {
            for (int i = 0; i < count; ++i) {
                TestResult placeholder;
                placeholder.status = status;
                placeholder.runId = run.runId;
                run.results.push_back(std::move(placeholder));
            }
        };
        append(TestStatus::Passed, passed);
        append(TestStatus::Failed, failed);
        append(TestStatus::Skipped, skipped);
        append(TestStatus::Error, errors);
        append(TestStatus::Timeout, timeouts);
        // Anything unaccounted for (an interrupted run) shows as errors.
        const int accounted = passed + failed + skipped + errors + timeouts;
        append(TestStatus::Error, std::max(0, total - accounted));

        runs.push_back(std::move(run));
    }
    return runs;
}

std::vector<TestHistoryEntry> SqliteResultRepository::testHistory(const std::string& testId,
                                                                  int limit) {
    const std::lock_guard<std::mutex> lock(mutex_);
    db::Statement query(*connection_,
                        "SELECT run_id, status, failure_category, start_time_ms, duration_ms, "
                        "       error_message "
                        "FROM test_results WHERE test_id = ? "
                        "ORDER BY start_time_ms DESC LIMIT ?;");
    query.bindAll(testId, std::max(1, limit));

    std::vector<TestHistoryEntry> entries;
    while (query.step()) {
        TestHistoryEntry entry;
        entry.runId = query.getText(0);
        if (const std::optional<TestStatus> status = testStatusFromString(query.getText(1));
            status.has_value()) {
            entry.status = *status;
        }
        if (const std::optional<FailureCategory> category =
                failureCategoryFromString(query.getText(2));
            category.has_value()) {
            entry.failureCategory = *category;
        }
        entry.startTime = fromEpochMillis(query.getInt(3));
        entry.duration = Milliseconds{query.getInt(4)};
        entry.errorMessage = query.getText(5);
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::vector<TestHistorySummary> SqliteResultRepository::historySummaries(int limit) {
    const std::lock_guard<std::mutex> lock(mutex_);

    db::Statement query(*connection_,
                        "SELECT test_id, qualified_name, COUNT(*) AS runs, "
                        "       SUM(status = 'PASSED') AS passes, "
                        "       SUM(status IN ('FAILED','ERROR','TIMEOUT')) AS failures, "
                        "       SUM(status = 'SKIPPED') AS skips, "
                        "       AVG(duration_ms) AS avg_ms, "
                        "       MAX(start_time_ms) AS last_ms "
                        "FROM test_results "
                        "GROUP BY test_id, qualified_name "
                        "ORDER BY last_ms DESC LIMIT ?;");
    query.bind(1, std::max(1, limit));

    std::vector<TestHistorySummary> summaries;
    while (query.step()) {
        TestHistorySummary summary;
        summary.testId = query.getText(0);
        summary.qualifiedName = query.getText(1);
        summary.totalRuns = static_cast<int>(query.getInt(2));
        summary.passes = static_cast<int>(query.getInt(3));
        summary.failures = static_cast<int>(query.getInt(4));
        summary.skips = static_cast<int>(query.getInt(5));
        summary.averageDuration = Milliseconds{static_cast<std::int64_t>(query.getDouble(6))};
        summary.lastRun = fromEpochMillis(query.getInt(7));
        summaries.push_back(std::move(summary));
    }

    // Flip rate and p95 need the ordered sequence of outcomes, which a GROUP BY
    // cannot give us. One extra query per test is acceptable for a listing that
    // is capped at `limit` rows and is not on any hot path.
    for (TestHistorySummary& summary : summaries) {
        db::Statement sequence(*connection_,
                               "SELECT status, duration_ms FROM test_results "
                               "WHERE test_id = ? ORDER BY start_time_ms DESC LIMIT 50;");
        sequence.bind(1, summary.testId);

        std::vector<bool> passSequence;
        std::vector<std::int64_t> durations;
        bool first = true;
        while (sequence.step()) {
            const std::optional<TestStatus> status = testStatusFromString(sequence.getText(0));
            if (!status.has_value() || *status == TestStatus::Skipped) {
                continue;  // skips are not evidence either way
            }
            if (first) {
                summary.lastStatus = *status;
                first = false;
            }
            passSequence.push_back(*status == TestStatus::Passed);
            durations.push_back(sequence.getInt(1));
        }

        if (passSequence.size() >= 2) {
            int flips = 0;
            for (std::size_t i = 1; i < passSequence.size(); ++i) {
                if (passSequence[i] != passSequence[i - 1]) {
                    ++flips;
                }
            }
            summary.flipRate =
                static_cast<double>(flips) / static_cast<double>(passSequence.size() - 1);
        }

        if (!durations.empty()) {
            std::sort(durations.begin(), durations.end());
            const std::size_t index =
                std::min(durations.size() - 1,
                         static_cast<std::size_t>(0.95 * static_cast<double>(durations.size())));
            summary.p95Duration = Milliseconds{durations[index]};
        }
    }

    return summaries;
}

json::Value SqliteResultRepository::aggregateStatistics(int runLimit) {
    const std::lock_guard<std::mutex> lock(mutex_);
    json::Value out = json::Value::object();
    const int limit = std::max(1, runLimit);

    {
        db::Statement query(*connection_,
                            "SELECT COUNT(*), COALESCE(SUM(total),0), COALESCE(SUM(passed),0), "
                            "       COALESCE(SUM(failed),0), COALESCE(SUM(skipped),0), "
                            "       COALESCE(SUM(errors),0), COALESCE(SUM(timeouts),0), "
                            "       COALESCE(AVG(duration_ms),0) "
                            "FROM (SELECT * FROM test_runs ORDER BY started_at_ms DESC LIMIT ?);");
        query.bind(1, limit);
        if (query.step()) {
            const std::int64_t total = query.getInt(1);
            const std::int64_t passed = query.getInt(2);
            const std::int64_t skipped = query.getInt(4);
            out.set("runs", query.getInt(0));
            out.set("total_tests", total);
            out.set("passed", passed);
            out.set("failed", query.getInt(3));
            out.set("skipped", skipped);
            out.set("errors", query.getInt(5));
            out.set("timeouts", query.getInt(6));
            out.set("average_run_duration_ms", static_cast<std::int64_t>(query.getDouble(7)));
            const std::int64_t considered = total - skipped;
            out.set("success_rate",
                    considered > 0
                        ? 100.0 * static_cast<double>(passed) / static_cast<double>(considered)
                        : 0.0);
        }
    }

    {
        db::Statement query(*connection_,
                            "SELECT failure_category, COUNT(*) AS n FROM test_results "
                            "WHERE failure_category != 'NONE' "
                            "GROUP BY failure_category ORDER BY n DESC;");
        json::Value categories = json::Value::array();
        while (query.step()) {
            json::Value entry = json::Value::object();
            entry.set("category", query.getText(0));
            entry.set("count", query.getInt(1));
            categories.push(entry);
        }
        out.set("failure_categories", categories);
    }

    {
        db::Statement query(*connection_,
                            "SELECT qualified_name, COUNT(*) AS n FROM test_results "
                            "WHERE status IN ('FAILED','ERROR','TIMEOUT') "
                            "GROUP BY qualified_name ORDER BY n DESC LIMIT 10;");
        json::Value tests = json::Value::array();
        while (query.step()) {
            json::Value entry = json::Value::object();
            entry.set("test", query.getText(0));
            entry.set("failures", query.getInt(1));
            tests.push(entry);
        }
        out.set("top_failing_tests", tests);
    }

    {
        db::Statement query(*connection_,
                            "SELECT qualified_name, AVG(duration_ms) AS d FROM test_results "
                            "GROUP BY qualified_name ORDER BY d DESC LIMIT 10;");
        json::Value tests = json::Value::array();
        while (query.step()) {
            json::Value entry = json::Value::object();
            entry.set("test", query.getText(0));
            entry.set("average_duration_ms", static_cast<std::int64_t>(query.getDouble(1)));
            tests.push(entry);
        }
        out.set("slowest_tests", tests);
    }

    {
        db::Statement query(*connection_,
                            "SELECT run_id, started_at_ms, total, passed, failed, skipped, "
                            "       errors, timeouts, duration_ms "
                            "FROM test_runs ORDER BY started_at_ms DESC LIMIT ?;");
        query.bind(1, limit);
        json::Value trend = json::Value::array();
        while (query.step()) {
            json::Value entry = json::Value::object();
            entry.set("run_id", query.getText(0));
            entry.set("started_at", toIso8601(fromEpochMillis(query.getInt(1))));
            entry.set("total", query.getInt(2));
            entry.set("passed", query.getInt(3));
            entry.set("failed", query.getInt(4));
            entry.set("skipped", query.getInt(5));
            entry.set("errors", query.getInt(6));
            entry.set("timeouts", query.getInt(7));
            entry.set("duration_ms", query.getInt(8));
            trend.push(entry);
        }
        out.set("recent_runs", trend);
    }

    return out;
}

int SqliteResultRepository::pruneOlderThan(int days) {
    if (days <= 0) {
        return 0;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::int64_t cutoff =
        toEpochMillis(WallClock::now()) - static_cast<std::int64_t>(days) * 86'400'000LL;

    db::Transaction transaction(*connection_);
    // ON DELETE CASCADE removes the results and events with the run.
    db::Statement remove(*connection_, "DELETE FROM test_runs WHERE started_at_ms < ?;");
    remove.bind(1, cutoff);
    remove.execute();
    const int removed = connection_->changes();
    transaction.commit();

    if (removed > 0) {
        connection_->executeScript("VACUUM;");
    }
    return removed;
}

json::Value SqliteResultRepository::storageInfo() {
    const std::lock_guard<std::mutex> lock(mutex_);
    json::Value out = json::Value::object();
    out.set("path", path_);
    out.set("sqlite_version", db::Connection::libraryVersion());
    out.set("journal_mode", connection_->pragma("journal_mode"));

    const auto count = [this](std::string_view sql) -> std::int64_t {
        db::Statement query(*connection_, sql);
        return query.step() ? query.getInt(0) : 0;
    };
    out.set("run_count", count("SELECT COUNT(*) FROM test_runs;"));
    out.set("result_count", count("SELECT COUNT(*) FROM test_results;"));
    out.set("event_count", count("SELECT COUNT(*) FROM test_events;"));

    db::Statement pageInfo(*connection_,
                           "SELECT (SELECT * FROM pragma_page_count()) * "
                           "       (SELECT * FROM pragma_page_size());");
    if (pageInfo.step()) {
        out.set("size_bytes", pageInfo.getInt(0));
    }
    return out;
}

// ---------------------------------------------------------------------------
// History helpers declared in ResultRepository.hpp
// ---------------------------------------------------------------------------

json::Value TestHistoryEntry::toJson() const {
    json::Value out = json::Value::object();
    out.set("run_id", runId);
    out.set("status", std::string(toString(status)));
    out.set("failure_category", std::string(toString(failureCategory)));
    out.set("start_time", toIso8601(startTime));
    out.set("duration_ms", millisOf(duration));
    out.set("error_message", errorMessage);
    return out;
}

bool TestHistorySummary::looksFlaky(int minimumRuns, double threshold) const {
    // Heuristic, and only ever presented as one. Requiring a minimum number of
    // runs keeps a single pass/fail pair (flip rate 1.0) from being announced
    // as a flaky test.
    return totalRuns >= minimumRuns && passes > 0 && failures > 0 && flipRate >= threshold;
}

json::Value TestHistorySummary::toJson() const {
    json::Value out = json::Value::object();
    out.set("test_id", testId);
    out.set("test", qualifiedName);
    out.set("total_runs", totalRuns);
    out.set("passes", passes);
    out.set("failures", failures);
    out.set("skips", skips);
    out.set("average_duration_ms", millisOf(averageDuration));
    out.set("p95_duration_ms", millisOf(p95Duration));
    out.set("last_run", toIso8601(lastRun));
    out.set("last_status", std::string(toString(lastStatus)));
    out.set("flip_rate", flipRate);
    out.set("flaky_candidate", looksFlaky());
    return out;
}

}  // namespace testforge
