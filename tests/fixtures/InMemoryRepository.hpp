#pragma once

#include "testforge/core/Exceptions.hpp"
#include "testforge/persistence/ResultRepository.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace testforge::testing {

/// A ResultRepository that keeps everything in memory.
///
/// This exists to prove the point of the ResultRepository interface: the
/// execution engine can be tested end to end with no SQLite, no file system
/// and no schema. It also records call counts, so a test can assert that the
/// runner persisted results *as it went* rather than in one batch at the end —
/// which is the behaviour that makes an interrupted run still useful.
class InMemoryRepository final : public ResultRepository {
 public:
    void initialise() override {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++initialiseCalls;
    }

    void beginRun(const TestRun& run) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++beginRunCalls;
        runs_[run.runId] = run;
        runOrder_.push_back(run.runId);
    }

    void saveResult(const TestResult& result) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++saveResultCalls;
        if (failNextSave) {
            failNextSave = false;
            throw PersistenceError("injected persistence failure");
        }
        results_[result.runId].push_back(result);
    }

    void completeRun(const TestRun& run) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++completeRunCalls;
        runs_[run.runId] = run;
    }

    void recordEvent(const std::string& runId,
                     const std::string& type,
                     const json::Value& payload) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back({runId, type, payload});
    }

    [[nodiscard]] std::optional<TestRun> loadRun(const std::string& runId) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = runs_.find(runId);
        if (found == runs_.end()) {
            return std::nullopt;
        }
        TestRun run = found->second;
        run.results = results_[runId];
        return run;
    }

    [[nodiscard]] std::vector<TestResult> loadResults(const std::string& runId) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        return results_[runId];
    }

    [[nodiscard]] std::vector<TestRun> listRuns(const RunQuery& query) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TestRun> out;
        for (auto it = runOrder_.rbegin(); it != runOrder_.rend(); ++it) {
            if (static_cast<int>(out.size()) >= query.limit) {
                break;
            }
            out.push_back(runs_[*it]);
        }
        return out;
    }

    [[nodiscard]] std::vector<TestHistoryEntry> testHistory(const std::string& testId,
                                                            int limit) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TestHistoryEntry> out;
        for (const auto& [runId, results] : results_) {
            for (const TestResult& result : results) {
                if (result.testId != testId) {
                    continue;
                }
                TestHistoryEntry entry;
                entry.runId = runId;
                entry.status = result.status;
                entry.failureCategory = result.failureCategory;
                entry.startTime = result.startTime;
                entry.duration = result.duration;
                entry.errorMessage = result.errorMessage;
                out.push_back(entry);
                if (static_cast<int>(out.size()) >= limit) {
                    return out;
                }
            }
        }
        return out;
    }

    [[nodiscard]] std::vector<TestHistorySummary> historySummaries(int /*limit*/) override {
        return {};
    }

    [[nodiscard]] json::Value aggregateStatistics(int /*runLimit*/) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        json::Value out = json::Value::object();
        out.set("runs", static_cast<std::int64_t>(runs_.size()));
        return out;
    }

    int pruneOlderThan(int /*days*/) override { return 0; }

    [[nodiscard]] json::Value storageInfo() override {
        const std::lock_guard<std::mutex> lock(mutex_);
        json::Value out = json::Value::object();
        out.set("path", ":memory:");
        out.set("run_count", static_cast<std::int64_t>(runs_.size()));
        return out;
    }

    // --- test-only inspection ---------------------------------------------

    struct Event {
        std::string runId;
        std::string type;
        json::Value payload;
    };

    [[nodiscard]] std::vector<Event> events() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

    [[nodiscard]] std::size_t runCount() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return runs_.size();
    }

    /// Makes the next saveResult throw, for testing that a persistence outage
    /// does not take the run down with it.
    bool failNextSave = false;

    int initialiseCalls = 0;
    int beginRunCalls = 0;
    int saveResultCalls = 0;
    int completeRunCalls = 0;

 private:
    mutable std::mutex mutex_;
    std::map<std::string, TestRun> runs_;
    std::map<std::string, std::vector<TestResult>> results_;
    std::vector<std::string> runOrder_;
    std::vector<Event> events_;
};

}  // namespace testforge::testing
