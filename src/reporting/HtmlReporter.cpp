#include "testforge/core/StringUtils.hpp"
#include "testforge/execution/FailureClassifier.hpp"
#include "testforge/reporting/Reporter.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace testforge::reporting {
namespace {

/// All styling is inlined. The report must open from a file:// URL on a
/// machine with no network, and survive being attached to an email or a CI
/// artefact — an external stylesheet would break all three.
constexpr std::string_view kStyle = R"CSS(
:root{--bg:#0f1116;--panel:#171a21;--line:#262b36;--fg:#e6e8ee;--muted:#9aa3b2;
--pass:#3fb950;--fail:#f85149;--skip:#8b949e;--err:#bc8cff;--time:#d29922;--accent:#58a6ff}
@media (prefers-color-scheme: light){
:root{--bg:#f6f8fa;--panel:#fff;--line:#d8dee4;--fg:#1f2328;--muted:#59636e;
--pass:#1a7f37;--fail:#cf222e;--skip:#6e7781;--err:#8250df;--time:#9a6700;--accent:#0969da}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
font:14px/1.55 ui-sans-serif,-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
.wrap{max-width:1100px;margin:0 auto;padding:32px 20px 80px}
h1{font-size:24px;margin:0 0 4px}
h2{font-size:17px;margin:36px 0 12px;padding-bottom:6px;border-bottom:1px solid var(--line)}
.sub{color:var(--muted);font-size:13px;margin-bottom:24px}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:12px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px 16px}
.card .n{font-size:26px;font-weight:650;line-height:1.1}
.card .l{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.04em;
margin-top:4px}
.pass{color:var(--pass)}.fail{color:var(--fail)}.skip{color:var(--skip)}
.err{color:var(--err)}.time{color:var(--time)}
.bar{height:10px;border-radius:5px;background:var(--line);overflow:hidden;margin:14px 0 6px;
display:flex}
.bar>span{display:block;height:100%}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{text-align:left;padding:7px 10px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--muted);font-weight:600;font-size:12px;text-transform:uppercase;
letter-spacing:.04em}
td.num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.badge{display:inline-block;padding:1px 8px;border-radius:11px;font-size:11px;font-weight:650;
border:1px solid currentColor}
details{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:10px 14px;
margin-bottom:10px}
details>summary{cursor:pointer;font-weight:600}
pre{background:var(--bg);border:1px solid var(--line);border-radius:6px;padding:10px;
overflow-x:auto;font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
white-space:pre-wrap;word-break:break-word}
.kv{display:grid;grid-template-columns:130px 1fr;gap:2px 12px;margin:10px 0;font-size:13px}
.kv dt{color:var(--muted)}
.kv dd{margin:0}
.note{color:var(--muted);font-size:12px;font-style:italic}
footer{margin-top:48px;color:var(--muted);font-size:12px;border-top:1px solid var(--line);
padding-top:14px}
)CSS";

std::string statusClass(TestStatus status) {
    switch (status) {
        case TestStatus::Passed:
            return "pass";
        case TestStatus::Failed:
            return "fail";
        case TestStatus::Skipped:
            return "skip";
        case TestStatus::Error:
            return "err";
        case TestStatus::Timeout:
            return "time";
    }
    return "";
}

std::string percentText(double value, int precision = 1) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(precision);
    os << value << '%';
    return os.str();
}

void writeCard(std::ostringstream& os,
               int value,
               std::string_view label,
               std::string_view cssClass) {
    os << "<div class=\"card\"><div class=\"n " << cssClass << "\">" << value
       << "</div><div class=\"l\">" << label << "</div></div>";
}

/// A stacked bar showing the status mix. Widths are percentages of the total.
void writeStatusBar(std::ostringstream& os, const RunStatistics& stats) {
    if (stats.total == 0) {
        return;
    }
    const auto slice = [&os, &stats](int count, std::string_view color) {
        if (count <= 0) {
            return;
        }
        const double width = 100.0 * static_cast<double>(count) / static_cast<double>(stats.total);
        os << "<span style=\"width:" << percentText(width, 2) << ";background:var(--" << color
           << ")\"></span>";
    };
    os << "<div class=\"bar\">";
    slice(stats.passed, "pass");
    slice(stats.failed, "fail");
    slice(stats.timeouts, "time");
    slice(stats.errors, "err");
    slice(stats.skipped, "skip");
    os << "</div>";
}

}  // namespace

std::string HtmlReporter::render(const TestRun& run) const {
    const RunStatistics stats = run.statistics();
    const auto esc = [](std::string_view text) { return strings::escapeHtml(text); };

    std::ostringstream os;
    os << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
       << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
       << "<title>" << esc(options_.title) << " - " << esc(run.runId) << "</title>\n"
       << "<style>" << kStyle << "</style>\n</head>\n<body>\n<div class=\"wrap\">\n";

    os << "<h1>" << esc(options_.title) << "</h1>\n";
    os << "<div class=\"sub\">run <code>" << esc(run.runId) << "</code>";
    if (!run.label.empty()) {
        os << " &middot; " << esc(run.label);
    }
    os << " &middot; " << esc(toIso8601(run.startedAt)) << " &middot; "
       << esc(formatDuration(run.duration)) << " on " << run.workers << " worker"
       << (run.workers == 1 ? "" : "s") << "</div>\n";

    // ----- summary cards -----
    os << "<div class=\"cards\">";
    writeCard(os, stats.total, "Total", "");
    writeCard(os, stats.passed, "Passed", "pass");
    writeCard(os, stats.failed, "Failed", "fail");
    writeCard(os, stats.timeouts, "Timeouts", "time");
    writeCard(os, stats.errors, "Errors", "err");
    writeCard(os, stats.skipped, "Skipped", "skip");
    os << "</div>\n";

    writeStatusBar(os, stats);
    os << "<div class=\"sub\">success rate <strong>" << percentText(stats.successRate())
       << "</strong>";
    if (stats.skipped > 0) {
        os << " <span class=\"note\">(skipped tests excluded)</span>";
    }
    os << "</div>\n";

    // ----- environment -----
    os << "<h2>Run context</h2>\n<dl class=\"kv\">";
    os << "<dt>Filter</dt><dd><code>" << esc(run.filterDescription) << "</code></dd>";
    os << "<dt>Host</dt><dd>" << esc(run.hostname) << "</dd>";
    if (!run.gitCommit.empty()) {
        os << "<dt>Commit</dt><dd><code>" << esc(run.gitCommit) << "</code></dd>";
    }
    os << "<dt>Started</dt><dd>" << esc(toIso8601(run.startedAt)) << "</dd>";
    os << "<dt>Finished</dt><dd>" << esc(toIso8601(run.finishedAt)) << "</dd>";
    os << "<dt>Average</dt><dd>" << esc(formatDuration(stats.averageDuration)) << "</dd>";
    os << "<dt>Median / p95</dt><dd>" << esc(formatDuration(stats.medianDuration)) << " / "
       << esc(formatDuration(stats.p95Duration)) << "</dd>";
    if (!stats.slowestTest.empty()) {
        os << "<dt>Slowest</dt><dd>" << esc(stats.slowestTest) << " ("
           << esc(formatDuration(stats.maxDuration)) << ")</dd>";
    }
    if (run.workers > 1 && stats.wallDuration.count() > 0) {
        std::ostringstream ratio;
        ratio.setf(std::ios::fixed);
        ratio.precision(2);
        ratio << stats.parallelEfficiency() << "x";
        os << "<dt>Parallel gain</dt><dd>" << ratio.str()
           << " <span class=\"note\">sum of test durations divided by wall clock</span></dd>";
    }
    os << "</dl>\n";

    // ----- failure categories -----
    if (!stats.failureCategoryCounts.empty()) {
        std::vector<std::pair<FailureCategory, int>> ranked(stats.failureCategoryCounts.begin(),
                                                            stats.failureCategoryCounts.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });

        os << "<h2>Failure categories</h2>\n<table><thead><tr><th>Category</th>"
           << "<th class=\"num\">Count</th><th>What it means</th></tr></thead><tbody>";
        for (const auto& [category, count] : ranked) {
            os << "<tr><td><span class=\"badge fail\">" << esc(toString(category))
               << "</span></td><td class=\"num\">" << count << "</td><td class=\"note\">"
               << esc(FailureClassifier::explain(category)) << "</td></tr>";
        }
        os << "</tbody></table>\n";
    }

    // ----- failures in detail -----
    std::vector<const TestResult*> failures;
    for (const TestResult& result : run.results) {
        if (isFailure(result.status)) {
            failures.push_back(&result);
        }
    }

    if (!failures.empty()) {
        os << "<h2>Failures (" << failures.size() << ")</h2>\n";
        for (const TestResult* result : failures) {
            os << "<details open><summary><span class=\"badge " << statusClass(result->status)
               << "\">" << esc(toString(result->status)) << "</span> "
               << esc(result->qualifiedName()) << "</summary>\n";

            os << "<dl class=\"kv\">";
            os << "<dt>Category</dt><dd>" << esc(toString(result->failureCategory)) << "</dd>";
            os << "<dt>Meaning</dt><dd class=\"note\">"
               << esc(FailureClassifier::explain(result->failureCategory)) << "</dd>";
            os << "<dt>Duration</dt><dd>" << esc(formatDuration(result->duration)) << "</dd>";
            os << "<dt>Worker</dt><dd>" << esc(result->worker) << "</dd>";
            if (result->attempt > 1) {
                os << "<dt>Attempt</dt><dd>" << result->attempt << "</dd>";
            }
            os << "</dl>";

            if (!result->errorMessage.empty()) {
                os << "<pre>" << esc(result->errorMessage);
                if (!result->errorDetail.empty()) {
                    os << '\n' << esc(result->errorDetail);
                }
                os << "</pre>";
            }

            if (!result->metadata.empty()) {
                os << "<details><summary>Recorded facts</summary><pre>"
                   << esc(result->metadata.dump(2)) << "</pre></details>";
            }

            if (options_.includeLogs && !result->logs.empty()) {
                os << "<details><summary>Log tail (" << result->logs.size()
                   << " lines)</summary><pre>";
                const std::size_t start =
                    result->logs.size() > static_cast<std::size_t>(options_.maxLogLines)
                        ? result->logs.size() - static_cast<std::size_t>(options_.maxLogLines)
                        : 0;
                for (std::size_t i = start; i < result->logs.size(); ++i) {
                    os << esc(result->logs[i]) << '\n';
                }
                os << "</pre></details>";
            }

            if (options_.includeDiagnostics && !result->diagnostics.empty()) {
                os << "<details><summary>System diagnostics at failure time</summary>";
                if (const json::Value* gpu = result->diagnostics.path("gpu.is_mock_data");
                    gpu != nullptr && gpu->boolOr(false)) {
                    os << "<p class=\"note\"><strong>Note:</strong> the GPU section below is "
                          "MOCK TEST DATA, not a reading from real hardware.</p>";
                }
                os << "<pre>" << esc(result->diagnostics.dump(2)) << "</pre></details>";
            }

            os << "</details>\n";
        }
    }

    // ----- all results -----
    os << "<h2>All results</h2>\n<table><thead><tr><th>Status</th><th>Test</th>"
       << "<th>Category</th><th class=\"num\">Duration</th><th>Message</th></tr></thead><tbody>";
    for (const TestResult& result : run.results) {
        os << "<tr><td><span class=\"badge " << statusClass(result.status) << "\">"
           << esc(toString(result.status)) << "</span></td>"
           << "<td>" << esc(result.qualifiedName()) << "</td>"
           << "<td>"
           << (result.failureCategory == FailureCategory::None
                   ? std::string("&mdash;")
                   : esc(toString(result.failureCategory)))
           << "</td>"
           << "<td class=\"num\">" << esc(formatDuration(result.duration)) << "</td>"
           << "<td>" << esc(strings::truncate(result.errorMessage, 160)) << "</td></tr>";
    }
    os << "</tbody></table>\n";

    os << "<footer>Generated by TestForge &middot; " << esc(toIso8601(WallClock::now()))
       << "<br>Failure categories and flaky-test flags are heuristics; see "
       << "docs/limitations.md.</footer>\n";
    os << "</div>\n</body>\n</html>\n";
    return os.str();
}

}  // namespace testforge::reporting
