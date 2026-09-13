#include "testforge/ai/FailureContext.hpp"

#include "testforge/core/StringUtils.hpp"
#include "testforge/execution/FailureClassifier.hpp"

#include <algorithm>
#include <array>

namespace testforge::ai {
namespace {

/// Metadata keys that are useful for diagnosis. An allow-list rather than a
/// deny-list: a test can record arbitrary metadata, and an allow-list means a
/// new key cannot leak by default.
constexpr std::array<std::string_view, 12> kAllowedMetadataKeys = {"http_method",
                                                                   "http_url",
                                                                   "http_status",
                                                                   "http_status_text",
                                                                   "response_time_ms",
                                                                   "content_type",
                                                                   "response_bytes",
                                                                   "transport_error",
                                                                   "response_excerpt",
                                                                   "timeout_ms",
                                                                   "attempt",
                                                                   "missing_dependency"};

/// Diagnostic paths worth including. Same reasoning as above, and it keeps the
/// document small enough to be cheap.
json::Value distillDiagnostics(const json::Value& diagnostics) {
    json::Value out = json::Value::object();
    if (diagnostics.empty()) {
        return out;
    }

    if (const json::Value* os = diagnostics.path("system.os"); os != nullptr) {
        json::Value summary = json::Value::object();
        for (const std::string_view key :
             {"distribution", "kernel_version", "architecture", "inside_container"}) {
            if (const json::Value* value = os->find(key); value != nullptr) {
                summary.set(std::string(key), *value);
            }
        }
        out.set("os", summary);
    }

    if (const json::Value* cpu = diagnostics.path("system.cpu"); cpu != nullptr) {
        json::Value summary = json::Value::object();
        for (const std::string_view key :
             {"logical_cores", "load_average_1m", "utilization_percent"}) {
            if (const json::Value* value = cpu->find(key); value != nullptr) {
                summary.set(std::string(key), *value);
            }
        }
        out.set("cpu", summary);
    }

    if (const json::Value* memory = diagnostics.path("system.memory"); memory != nullptr) {
        json::Value summary = json::Value::object();
        for (const std::string_view key : {"total_kb", "available_kb", "used_percent"}) {
            if (const json::Value* value = memory->find(key); value != nullptr) {
                summary.set(std::string(key), *value);
            }
        }
        out.set("memory", summary);
    }

    // Only filesystems that are actually short of space: a model given twelve
    // healthy mounts will find a pattern in them whether or not one exists.
    if (const json::Value* disks = diagnostics.path("system.disks");
        disks != nullptr && disks->isArray()) {
        json::Value tight = json::Value::array();
        for (const json::Value& disk : disks->asArray()) {
            if (const json::Value* used = disk.find("used_percent");
                used != nullptr && used->doubleOr(0.0) >= 85.0) {
                tight.push(disk);
            }
        }
        if (!tight.empty()) {
            out.set("disks_under_pressure", tight);
        }
    }

    if (const json::Value* gpu = diagnostics.find("gpu"); gpu != nullptr) {
        json::Value summary = json::Value::object();
        for (const std::string_view key :
             {"available", "is_mock_data", "summary", "unavailable_reason", "driver_version"}) {
            if (const json::Value* value = gpu->find(key); value != nullptr) {
                summary.set(std::string(key), *value);
            }
        }
        out.set("gpu", summary);
    }

    if (const json::Value* logs = diagnostics.find("system_logs");
        logs != nullptr && logs->isArray()) {
        json::Value trimmed = json::Value::array();
        const json::Array& lines = logs->asArray();
        const std::size_t start = lines.size() > 10 ? lines.size() - 10 : 0;
        for (std::size_t i = start; i < lines.size(); ++i) {
            trimmed.push(lines[i]);
        }
        out.set("recent_system_logs", trimmed);
    }

    return out;
}

}  // namespace

json::Value FailureContext::build(const TestResult& result) const {
    json::Value context = json::Value::object();

    // Stated up front so the receiving prompt can quote it back into the model
    // instructions: everything below is evidence, not direction.
    context.set("content_type", "testforge.failure_context");
    context.set("trust_level", "untrusted_data");

    json::Value test = json::Value::object();
    test.set("name", result.qualifiedName());
    test.set("suite", result.suiteName);
    json::Value tags = json::Value::array();
    for (const std::string& tag : result.tags) {
        tags.push(tag);
    }
    test.set("tags", tags);
    context.set("test", test);

    json::Value outcome = json::Value::object();
    outcome.set("status", std::string(toString(result.status)));
    outcome.set("failure_category", std::string(toString(result.failureCategory)));
    outcome.set("category_meaning",
                std::string(FailureClassifier::explain(result.failureCategory)));
    outcome.set("duration_ms", millisOf(result.duration));
    outcome.set("attempt", result.attempt);
    outcome.set("error_message",
                strings::truncate(strings::redactSecrets(result.errorMessage), 1000));
    outcome.set(
        "error_detail",
        strings::truncate(strings::redactSecrets(result.errorDetail), limits_.maxErrorDetail));

    json::Value metadata = json::Value::object();
    for (const json::Member& member : result.metadata.asObject()) {
        const bool allowed =
            std::find(kAllowedMetadataKeys.begin(), kAllowedMetadataKeys.end(), member.first) !=
            kAllowedMetadataKeys.end();
        if (!allowed) {
            continue;
        }
        if (member.second.isString()) {
            const std::size_t limit =
                member.first == "response_excerpt" ? limits_.maxResponseExcerpt : std::size_t{500};
            metadata.set(
                member.first,
                strings::truncate(strings::redactSecrets(member.second.asString()), limit));
        } else {
            metadata.set(member.first, member.second);
        }
    }
    outcome.set("metadata", metadata);

    json::Value logs = json::Value::array();
    const std::size_t start =
        result.logs.size() > limits_.maxLogLines ? result.logs.size() - limits_.maxLogLines : 0;
    for (std::size_t i = start; i < result.logs.size(); ++i) {
        logs.push(
            strings::truncate(strings::redactSecrets(result.logs[i]), limits_.maxLogLineLength));
    }
    outcome.set("log_tail", logs);

    context.set("result", outcome);
    context.set("diagnostics", distillDiagnostics(result.diagnostics));

    // Final guard: even with per-field limits, a pathological result could add
    // up. Trim the largest optional sections rather than sending the lot.
    if (context.dump().size() > limits_.maxTotalBytes) {
        if (json::Value* trimmedResult = context.find("result"); trimmedResult != nullptr) {
            trimmedResult->set("log_tail", json::Value::array());
            trimmedResult->set("error_detail", strings::truncate(result.errorMessage, 500));
        }
        context.set("truncated", true);
        context.set("truncation_note",
                    "The log tail and error detail were removed because the context exceeded "
                    "the configured size budget.");
    }

    return context;
}

json::Value FailureContext::withRunContext(json::Value context, const TestRun& run) const {
    json::Value runInfo = json::Value::object();
    runInfo.set("run_id", run.runId);
    runInfo.set("started_at", toIso8601(run.startedAt));
    runInfo.set("workers", run.workers);
    runInfo.set("hostname", run.hostname);
    if (!run.gitCommit.empty()) {
        runInfo.set("git_commit", run.gitCommit);
    }

    // How much of the run failed is decisive context: one red test among 200
    // green ones is a different problem from 200 red tests.
    const RunStatistics stats = run.statistics();
    json::Value totals = json::Value::object();
    totals.set("total", stats.total);
    totals.set("passed", stats.passed);
    totals.set("failed", stats.failed);
    totals.set("errors", stats.errors);
    totals.set("timeouts", stats.timeouts);
    runInfo.set("run_totals", totals);

    context.set("run", runInfo);
    return context;
}

json::Value FailureContext::withHistory(json::Value context, const json::Value& history) const {
    context.set("history", history);
    return context;
}

}  // namespace testforge::ai
