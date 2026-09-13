#include "testforge/reporting/Reporter.hpp"

namespace testforge::reporting {

std::string JsonReporter::render(const TestRun& run) const {
    json::Value root = run.toJson();
    // Reports are consumed by tooling that may be older than the producer;
    // a version field is what makes that survivable.
    root.set("report_format", "testforge.run");
    root.set("report_version", 1);
    return pretty_ ? root.dump(2) : root.dump();
}

}  // namespace testforge::reporting
