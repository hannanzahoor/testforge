#include "testforge/core/StringUtils.hpp"
#include "testforge/reporting/Reporter.hpp"

#include <map>
#include <sstream>

namespace testforge::reporting {
namespace {

std::string seconds(Milliseconds duration) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(3);
    os << static_cast<double>(duration.count()) / 1000.0;
    return os.str();
}

}  // namespace

std::string JUnitReporter::render(const TestRun& run) const {
    // JUnit XML groups by <testsuite>, so results are bucketed by TestForge
    // suite. CI systems use that grouping for their own summaries.
    std::map<std::string, std::vector<const TestResult*>> bySuite;
    for (const TestResult& result : run.results) {
        bySuite[result.suiteName.empty() ? "default" : result.suiteName].push_back(&result);
    }

    const RunStatistics overall = run.statistics();

    std::ostringstream os;
    os << R"(<?xml version="1.0" encoding="UTF-8"?>)" << '\n';
    os << "<testsuites name=\"TestForge\""
       << " tests=\"" << overall.total << "\""
       << " failures=\"" << overall.failed << "\""
       << " errors=\"" << (overall.errors + overall.timeouts) << "\""
       << " skipped=\"" << overall.skipped << "\""
       << " time=\"" << seconds(run.duration) << "\">\n";

    for (const auto& [suiteName, results] : bySuite) {
        int failures = 0;
        int errors = 0;
        int skipped = 0;
        std::int64_t totalMs = 0;
        for (const TestResult* result : results) {
            switch (result->status) {
                case TestStatus::Failed:
                    ++failures;
                    break;
                case TestStatus::Error:
                case TestStatus::Timeout:
                    ++errors;
                    break;
                case TestStatus::Skipped:
                    ++skipped;
                    break;
                case TestStatus::Passed:
                    break;
            }
            totalMs += result->duration.count();
        }

        os << "  <testsuite name=\"" << strings::escapeXml(suiteName) << "\""
           << " tests=\"" << results.size() << "\""
           << " failures=\"" << failures << "\""
           << " errors=\"" << errors << "\""
           << " skipped=\"" << skipped << "\""
           << " time=\"" << seconds(Milliseconds{totalMs}) << "\""
           << " timestamp=\"" << strings::escapeXml(toIso8601(run.startedAt)) << "\""
           << " hostname=\"" << strings::escapeXml(run.hostname) << "\">\n";

        for (const TestResult* result : results) {
            os << "    <testcase name=\"" << strings::escapeXml(result->testName) << "\""
               << " classname=\"" << strings::escapeXml(suiteName) << "\""
               << " time=\"" << seconds(result->duration) << "\">\n";

            const std::string detail =
                result->errorDetail.empty() ? result->errorMessage : result->errorDetail;

            switch (result->status) {
                case TestStatus::Failed:
                    os << "      <failure type=\""
                       << strings::escapeXml(std::string(toString(result->failureCategory)))
                       << "\" message=\"" << strings::escapeXml(result->errorMessage) << "\">"
                       << strings::escapeXml(detail) << "</failure>\n";
                    break;
                case TestStatus::Error:
                case TestStatus::Timeout:
                    os << "      <error type=\""
                       << strings::escapeXml(std::string(toString(result->failureCategory)))
                       << "\" message=\"" << strings::escapeXml(result->errorMessage) << "\">"
                       << strings::escapeXml(detail) << "</error>\n";
                    break;
                case TestStatus::Skipped:
                    os << "      <skipped message=\"" << strings::escapeXml(result->errorMessage)
                       << "\"/>\n";
                    break;
                case TestStatus::Passed:
                    break;
            }

            if (!result->logs.empty()) {
                os << "      <system-out>";
                for (const std::string& line : result->logs) {
                    os << strings::escapeXml(line) << '\n';
                }
                os << "</system-out>\n";
            }

            os << "    </testcase>\n";
        }
        os << "  </testsuite>\n";
    }

    os << "</testsuites>\n";
    return os.str();
}

}  // namespace testforge::reporting
