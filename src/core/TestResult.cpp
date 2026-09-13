#include "testforge/core/TestResult.hpp"

#include "testforge/core/Ids.hpp"
#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace testforge {
namespace {

json::Value stringsToJson(const std::vector<std::string>& items) {
    json::Value out = json::Value::array();
    for (const std::string& item : items) {
        out.push(item);
    }
    return out;
}

std::vector<std::string> stringsFromJson(const json::Value* value) {
    std::vector<std::string> out;
    if (value == nullptr || !value->isArray()) {
        return out;
    }
    for (const json::Value& item : value->asArray()) {
        if (item.isString()) {
            out.push_back(item.asString());
        }
    }
    return out;
}

std::string readString(const json::Value& obj, std::string_view key) {
    const json::Value* found = obj.find(key);
    return (found != nullptr && found->isString()) ? found->asString() : std::string{};
}

/// Nearest-rank percentile over a sorted vector. With small test counts this
/// returns a value that was actually observed, rather than interpolating
/// between samples that do not exist.
Milliseconds percentile(const std::vector<std::int64_t>& sorted, double fraction) {
    if (sorted.empty()) {
        return Milliseconds{0};
    }
    const auto rank =
        static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(sorted.size())));
    const std::size_t index = rank == 0 ? 0 : std::min(rank - 1, sorted.size() - 1);
    return Milliseconds{sorted[index]};
}

}  // namespace

std::string TestResult::qualifiedName() const {
    if (suiteName.empty()) {
        return testName;
    }
    return suiteName + "." + testName;
}

TestResult TestResult::make(std::string suite, std::string name, TestStatus status) {
    TestResult result;
    result.suiteName = std::move(suite);
    result.testName = std::move(name);
    result.testId = ids::testId(result.suiteName, result.testName);
    result.status = status;
    result.failureCategory = status == TestStatus::Passed || status == TestStatus::Skipped
                                 ? FailureCategory::None
                                 : FailureCategory::Unknown;
    result.startTime = WallClock::now();
    result.endTime = result.startTime;
    return result;
}

json::Value TestResult::toJson() const {
    json::Value out = json::Value::object();
    out.set("test_id", testId);
    out.set("test_name", testName);
    out.set("suite_name", suiteName);
    out.set("qualified_name", qualifiedName());
    out.set("run_id", runId);
    out.set("status", std::string(toString(status)));
    out.set("failure_category", std::string(toString(failureCategory)));
    out.set("start_time", toIso8601(startTime));
    out.set("end_time", toIso8601(endTime));
    out.set("duration_ms", millisOf(duration));
    out.set("error_message", errorMessage);
    out.set("error_detail", errorDetail);
    out.set("logs", stringsToJson(logs));
    out.set("tags", stringsToJson(tags));
    out.set("metadata", metadata);
    out.set("diagnostics", diagnostics);
    out.set("attempt", attempt);
    out.set("flaky_candidate", flakyCandidate);
    out.set("worker", worker);
    return out;
}

TestResult TestResult::fromJson(const json::Value& value) {
    TestResult result;
    result.testId = readString(value, "test_id");
    result.testName = readString(value, "test_name");
    result.suiteName = readString(value, "suite_name");
    result.runId = readString(value, "run_id");

    if (const std::optional<TestStatus> status = testStatusFromString(readString(value, "status"));
        status.has_value()) {
        result.status = *status;
    }
    if (const std::optional<FailureCategory> category =
            failureCategoryFromString(readString(value, "failure_category"));
        category.has_value()) {
        result.failureCategory = *category;
    }

    result.startTime = fromIso8601(readString(value, "start_time"));
    result.endTime = fromIso8601(readString(value, "end_time"));
    if (const json::Value* duration = value.find("duration_ms");
        duration != nullptr && duration->isNumber()) {
        result.duration = Milliseconds{duration->asInt()};
    }
    result.errorMessage = readString(value, "error_message");
    result.errorDetail = readString(value, "error_detail");
    result.logs = stringsFromJson(value.find("logs"));
    result.tags = stringsFromJson(value.find("tags"));
    if (const json::Value* metadata = value.find("metadata"); metadata != nullptr) {
        result.metadata = *metadata;
    }
    if (const json::Value* diagnostics = value.find("diagnostics"); diagnostics != nullptr) {
        result.diagnostics = *diagnostics;
    }
    if (const json::Value* attempt = value.find("attempt");
        attempt != nullptr && attempt->isNumber()) {
        result.attempt = static_cast<int>(attempt->asInt());
    }
    if (const json::Value* flaky = value.find("flaky_candidate");
        flaky != nullptr && flaky->isBool()) {
        result.flakyCandidate = flaky->asBool();
    }
    result.worker = readString(value, "worker");

    if (result.testId.empty()) {
        result.testId = ids::testId(result.suiteName, result.testName);
    }
    return result;
}

// ---------------------------------------------------------------------------
// RunStatistics
// ---------------------------------------------------------------------------

double RunStatistics::successRate() const {
    const int considered = total - skipped;
    if (considered <= 0) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(passed) / static_cast<double>(considered);
}

double RunStatistics::parallelEfficiency() const {
    if (wallDuration.count() <= 0) {
        return 0.0;
    }
    return static_cast<double>(totalDuration.count()) / static_cast<double>(wallDuration.count());
}

RunStatistics RunStatistics::compute(const std::vector<TestResult>& results,
                                     Milliseconds wallDuration) {
    RunStatistics stats;
    stats.wallDuration = wallDuration;
    stats.total = static_cast<int>(results.size());

    std::vector<std::int64_t> durations;
    durations.reserve(results.size());
    std::int64_t sum = 0;

    for (const TestResult& result : results) {
        switch (result.status) {
            case TestStatus::Passed:
                ++stats.passed;
                break;
            case TestStatus::Failed:
                ++stats.failed;
                break;
            case TestStatus::Skipped:
                ++stats.skipped;
                break;
            case TestStatus::Error:
                ++stats.errors;
                break;
            case TestStatus::Timeout:
                ++stats.timeouts;
                break;
        }

        if (result.failureCategory != FailureCategory::None) {
            ++stats.failureCategoryCounts[result.failureCategory];
        }

        const std::int64_t ms = result.duration.count();
        durations.push_back(ms);
        sum += ms;
        if (ms > stats.maxDuration.count()) {
            stats.maxDuration = Milliseconds{ms};
            stats.slowestTest = result.qualifiedName();
        }
    }

    stats.totalDuration = Milliseconds{sum};
    if (!durations.empty()) {
        stats.averageDuration = Milliseconds{sum / static_cast<std::int64_t>(durations.size())};
        std::sort(durations.begin(), durations.end());
        stats.medianDuration = percentile(durations, 0.50);
        stats.p95Duration = percentile(durations, 0.95);
    }

    return stats;
}

json::Value RunStatistics::toJson() const {
    json::Value out = json::Value::object();
    out.set("total", total);
    out.set("passed", passed);
    out.set("failed", failed);
    out.set("skipped", skipped);
    out.set("errors", errors);
    out.set("timeouts", timeouts);
    out.set("success_rate", successRate());
    out.set("total_duration_ms", millisOf(totalDuration));
    out.set("wall_duration_ms", millisOf(wallDuration));
    out.set("average_duration_ms", millisOf(averageDuration));
    out.set("median_duration_ms", millisOf(medianDuration));
    out.set("p95_duration_ms", millisOf(p95Duration));
    out.set("max_duration_ms", millisOf(maxDuration));
    out.set("slowest_test", slowestTest);
    out.set("parallel_efficiency", parallelEfficiency());

    json::Value categories = json::Value::object();
    for (const auto& [category, count] : failureCategoryCounts) {
        categories.set(std::string(toString(category)), count);
    }
    out.set("failure_categories", categories);
    return out;
}

// ---------------------------------------------------------------------------
// TestRun
// ---------------------------------------------------------------------------

RunStatistics TestRun::statistics() const {
    return RunStatistics::compute(results, duration);
}

json::Value TestRun::toSummaryJson() const {
    json::Value out = json::Value::object();
    out.set("run_id", runId);
    out.set("label", label);
    out.set("started_at", toIso8601(startedAt));
    out.set("finished_at", toIso8601(finishedAt));
    out.set("duration_ms", millisOf(duration));
    out.set("workers", workers);
    out.set("filter", filterDescription);
    out.set("git_commit", gitCommit);
    out.set("hostname", hostname);
    out.set("cancellation_reason", cancellationReason);
    out.set("statistics", statistics().toJson());
    return out;
}

json::Value TestRun::toJson() const {
    json::Value out = toSummaryJson();
    out.set("environment", environment);
    json::Value items = json::Value::array();
    for (const TestResult& result : results) {
        items.push(result.toJson());
    }
    out.set("results", items);
    return out;
}

TestRun TestRun::fromJson(const json::Value& value) {
    TestRun run;
    run.runId = readString(value, "run_id");
    run.label = readString(value, "label");
    run.startedAt = fromIso8601(readString(value, "started_at"));
    run.finishedAt = fromIso8601(readString(value, "finished_at"));
    if (const json::Value* duration = value.find("duration_ms");
        duration != nullptr && duration->isNumber()) {
        run.duration = Milliseconds{duration->asInt()};
    }
    if (const json::Value* workers = value.find("workers");
        workers != nullptr && workers->isNumber()) {
        run.workers = static_cast<int>(workers->asInt());
    }
    run.filterDescription = readString(value, "filter");
    run.gitCommit = readString(value, "git_commit");
    run.hostname = readString(value, "hostname");
    run.cancellationReason = readString(value, "cancellation_reason");
    if (const json::Value* environment = value.find("environment"); environment != nullptr) {
        run.environment = *environment;
    }
    if (const json::Value* results = value.find("results");
        results != nullptr && results->isArray()) {
        for (const json::Value& item : results->asArray()) {
            run.results.push_back(TestResult::fromJson(item));
        }
    }
    return run;
}

int TestRun::exitCode() const {
    // 2 is reserved for "TestForge itself is broken". A test that hits a
    // configuration or environment problem is still a test result, and
    // reporting it as a tool failure would send CI looking in the wrong place.
    for (const TestResult& result : results) {
        if (result.failureCategory == FailureCategory::FrameworkError) {
            return 2;
        }
    }
    for (const TestResult& result : results) {
        if (isFailure(result.status)) {
            return 1;
        }
    }
    // Nothing failed — but if the run was cut short, it did not pass either.
    // The tests it never reached are unverified, and a zero here would tell CI
    // they were fine. Failures still take precedence: they are the more
    // actionable fact.
    if (wasCancelled()) {
        return 4;
    }
    return 0;
}

}  // namespace testforge
