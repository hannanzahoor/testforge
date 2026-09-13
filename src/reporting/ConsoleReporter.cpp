#include "testforge/core/StringUtils.hpp"
#include "testforge/execution/FailureClassifier.hpp"
#include "testforge/reporting/Reporter.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace testforge::reporting {
namespace {

std::string colorize(std::string_view text, std::string_view color, bool enabled) {
    if (!enabled || color.empty()) {
        return std::string(text);
    }
    return std::string(color) + std::string(text) + std::string(kAnsiReset);
}

std::string statusGlyph(TestStatus status) {
    switch (status) {
        case TestStatus::Passed:
            return "PASS";
        case TestStatus::Failed:
            return "FAIL";
        case TestStatus::Skipped:
            return "SKIP";
        case TestStatus::Error:
            return "ERR ";
        case TestStatus::Timeout:
            return "TIME";
    }
    return "????";
}

/// A fixed-width bar, e.g. "[##########----------]".
std::string progressBar(double fraction, int width) {
    const int filled = std::clamp(static_cast<int>(fraction * width), 0, width);
    std::string bar = "[";
    bar.append(static_cast<std::size_t>(filled), '#');
    bar.append(static_cast<std::size_t>(width - filled), '-');
    bar.push_back(']');
    return bar;
}

}  // namespace

ConsoleReporter::ConsoleReporter(ConsoleReporterOptions options) : options_(options) {}

std::ostream& ConsoleReporter::out() const {
    return options_.stream != nullptr ? *options_.stream : std::cout;
}

void ConsoleReporter::onRunStarted(const TestRun& run, const std::vector<TestMetadata>& selected) {
    selectedCount_ = static_cast<int>(selected.size());
    completed_.store(0, std::memory_order_relaxed);
    if (!options_.live) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    out() << colorize("TestForge", kAnsiBold, options_.color) << "  run " << run.runId << '\n'
          << "  tests    " << selectedCount_ << '\n'
          << "  workers  " << run.workers << '\n'
          << "  filter   " << run.filterDescription << '\n'
          << std::string(72, '-') << '\n';
    out().flush();
}

void ConsoleReporter::onTestFinished(const TestResult& result) {
    if (!options_.live) {
        return;
    }
    const int index = completed_.fetch_add(1, std::memory_order_relaxed) + 1;

    // Workers call this concurrently; one lock keeps lines from interleaving.
    const std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream line;
    line << '[' << strings::padLeft(std::to_string(index), 4) << '/'
         << strings::padLeft(std::to_string(selectedCount_), 4) << "] ";
    out() << line.str()
          << colorize(statusGlyph(result.status), ansiColor(result.status), options_.color) << "  "
          << strings::padRight(result.qualifiedName(), 44) << ' '
          << strings::padLeft(formatDuration(result.duration), 9);

    if (result.status == TestStatus::Skipped && options_.showSkipReasons &&
        !result.errorMessage.empty()) {
        out() << "  " << colorize(result.errorMessage, kAnsiDim, options_.color);
    } else if (isFailure(result.status) && !result.errorMessage.empty()) {
        out() << "  " << strings::truncate(result.errorMessage, 120);
    }
    out() << '\n';
    out().flush();
}

void ConsoleReporter::onRunFinished(const TestRun& run) {
    if (!options_.live) {
        return;
    }
    print(run);
}

void ConsoleReporter::print(const TestRun& run) const {
    out() << render(run);
    out().flush();
}

std::string ConsoleReporter::render(const TestRun& run) const {
    const RunStatistics stats = run.statistics();
    const bool color = options_.color;
    std::ostringstream os;

    os << '\n' << std::string(72, '=') << '\n';
    os << colorize("TestForge Test Report", kAnsiBold, color) << '\n';
    os << std::string(72, '=') << '\n';
    os << "Run       " << run.runId << '\n';
    if (!run.label.empty()) {
        os << "Label     " << run.label << '\n';
    }
    os << "Started   " << toIso8601(run.startedAt) << '\n';
    os << "Duration  " << formatDuration(run.duration) << "  (" << run.workers << " worker"
       << (run.workers == 1 ? "" : "s") << ")\n";
    os << "Filter    " << run.filterDescription << '\n';
    if (!run.hostname.empty()) {
        os << "Host      " << run.hostname;
        if (!run.gitCommit.empty()) {
            os << "   commit " << run.gitCommit;
        }
        os << '\n';
    }

    os << '\n';
    os << "Total     " << stats.total << '\n';
    os << colorize("Passed    ", ansiColor(TestStatus::Passed), color) << stats.passed << '\n';
    os << colorize("Failed    ", ansiColor(TestStatus::Failed), color) << stats.failed << '\n';
    os << colorize("Skipped   ", ansiColor(TestStatus::Skipped), color) << stats.skipped << '\n';
    os << colorize("Errors    ", ansiColor(TestStatus::Error), color) << stats.errors << '\n';
    os << colorize("Timeouts  ", ansiColor(TestStatus::Timeout), color) << stats.timeouts << '\n';

    os << '\n';
    const double rate = stats.successRate();
    os << "Success   " << progressBar(rate / 100.0, 24) << ' ';
    {
        std::ostringstream percent;
        percent.setf(std::ios::fixed);
        percent.precision(1);
        percent << rate << '%';
        os << colorize(percent.str(),
                       rate >= 99.5 ? ansiColor(TestStatus::Passed)
                                    : (rate >= 90.0 ? "\033[33m" : ansiColor(TestStatus::Failed)),
                       color);
    }
    if (stats.skipped > 0) {
        os << "   (" << stats.skipped << " skipped, excluded from the rate)";
    }
    os << '\n';

    os << '\n';
    os << "Timing    average " << formatDuration(stats.averageDuration) << "   median "
       << formatDuration(stats.medianDuration) << "   p95 " << formatDuration(stats.p95Duration)
       << '\n';
    if (!stats.slowestTest.empty()) {
        os << "          slowest " << stats.slowestTest << " (" << formatDuration(stats.maxDuration)
           << ")\n";
    }
    if (stats.wallDuration.count() > 0 && run.workers > 1) {
        std::ostringstream speedup;
        speedup.setf(std::ios::fixed);
        speedup.precision(2);
        speedup << stats.parallelEfficiency() << "x";
        os << "          test-time / wall-time " << speedup.str() << " across " << run.workers
           << " workers\n";
    }

    if (!stats.failureCategoryCounts.empty()) {
        // Ranked, because the top category is where triage should start.
        std::vector<std::pair<FailureCategory, int>> ranked(stats.failureCategoryCounts.begin(),
                                                            stats.failureCategoryCounts.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });

        os << '\n' << colorize("Failure categories", kAnsiBold, color) << '\n';
        int rank = 1;
        for (const auto& [category, count] : ranked) {
            os << "  " << rank++ << ". " << strings::padRight(std::string(toString(category)), 24)
               << count << '\n';
        }
    }

    // ----- failure detail -----
    std::vector<const TestResult*> failures;
    for (const TestResult& result : run.results) {
        if (isFailure(result.status)) {
            failures.push_back(&result);
        }
    }

    if (!failures.empty() && options_.verboseFailures) {
        os << '\n' << std::string(72, '-') << '\n';
        os << colorize("Failures", kAnsiBold, color) << '\n';

        int shown = 0;
        for (const TestResult* result : failures) {
            if (shown++ >= options_.maxFailuresShown) {
                os << "\n  ... and " << (failures.size() - static_cast<std::size_t>(shown) + 1)
                   << " more (see the JSON or HTML report)\n";
                break;
            }
            os << '\n'
               << colorize(std::string(toString(result->status)), ansiColor(result->status), color)
               << "  " << colorize(result->qualifiedName(), kAnsiBold, color) << '\n';
            os << "  category   " << toString(result->failureCategory) << '\n';
            os << "  meaning    " << FailureClassifier::explain(result->failureCategory) << '\n';
            os << "  duration   " << formatDuration(result->duration);
            if (result->attempt > 1) {
                os << "   (attempt " << result->attempt << ")";
            }
            os << '\n';

            if (!result->errorMessage.empty()) {
                os << "  message    " << result->errorMessage << '\n';
            }
            if (!result->errorDetail.empty()) {
                for (const std::string& line : strings::splitLines(result->errorDetail)) {
                    os << "  " << line << '\n';
                }
            }

            // The HTTP exchange, when there was one, is usually the whole story.
            //
            // Every lookup goes through a safe reader. A test can record
            // http_status without http_url — the failure-injection suite does
            // exactly that — and a missing member must not become a null
            // dereference inside the reporter.
            if (result->metadata.contains("http_status")) {
                os << "  request    " << json::stringAt(result->metadata, "http_method", "?") << ' '
                   << json::stringAt(result->metadata, "http_url", "?") << " -> "
                   << json::intAt(result->metadata, "http_status") << '\n';

                const std::string excerpt = json::stringAt(result->metadata, "response_excerpt");
                if (!excerpt.empty()) {
                    os << "  response   " << strings::truncate(excerpt, 300) << '\n';
                }
            }

            if (!result->logs.empty()) {
                os << "  logs\n";
                const std::size_t start = result->logs.size() > 12 ? result->logs.size() - 12 : 0;
                for (std::size_t i = start; i < result->logs.size(); ++i) {
                    os << "    " << result->logs[i] << '\n';
                }
            }

            if (options_.showDiagnostics && !result->diagnostics.empty()) {
                const std::string gpu = json::stringAt(result->diagnostics, "gpu.summary");
                if (!gpu.empty()) {
                    os << "  gpu        " << gpu << '\n';
                }

                const double memoryUsed =
                    json::doubleAt(result->diagnostics, "system.memory.used_percent", -1.0);
                if (memoryUsed >= 0.0) {
                    std::ostringstream used;
                    used.setf(std::ios::fixed);
                    used.precision(1);
                    used << memoryUsed << '%';
                    os << "  memory     " << used.str() << " used at failure time\n";
                }

                const double load =
                    json::doubleAt(result->diagnostics, "system.cpu.load_average_1m", -1.0);
                if (load >= 0.0) {
                    os << "  load       " << load << " (1m)\n";
                }
            }
        }
    }

    // ----- flaky candidates -----
    std::vector<std::string> flaky;
    for (const TestResult& result : run.results) {
        if (result.flakyCandidate) {
            flaky.push_back(result.qualifiedName());
        }
    }
    if (!flaky.empty()) {
        os << '\n'
           << colorize("Flaky candidates", kAnsiBold, color)
           << " (passed only after a retry — heuristic)\n";
        for (const std::string& name : flaky) {
            os << "  - " << name << '\n';
        }
    }

    os << '\n' << std::string(72, '=') << '\n';
    const int exitCode = run.exitCode();
    if (exitCode == 4) {
        os << colorize("RESULT: INCOMPLETE", ansiColor(TestStatus::Skipped), color) << " — "
           << run.cancellationReason << "; " << stats.skipped << " test(s) were never run\n";
    } else if (exitCode == 0) {
        os << colorize("RESULT: PASS", ansiColor(TestStatus::Passed), color) << '\n';
    } else if (exitCode == 1) {
        os << colorize("RESULT: FAIL", ansiColor(TestStatus::Failed), color) << " — "
           << (stats.failed + stats.errors + stats.timeouts) << " test(s) need attention\n";
    } else {
        os << colorize("RESULT: ERROR", ansiColor(TestStatus::Error), color)
           << " — the framework or its configuration is at fault\n";
    }
    os << std::string(72, '=') << "\n\n";

    return os.str();
}

}  // namespace testforge::reporting
