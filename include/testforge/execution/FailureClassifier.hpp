#pragma once

#include "testforge/core/Json.hpp"
#include "testforge/core/Status.hpp"

#include <exception>
#include <string>
#include <string_view>

namespace testforge {

/// Decides what kind of failure just happened.
///
/// This is the step that turns "the test threw" into "the service is down" or
/// "the assertion is wrong" or "this machine is out of disk". Getting it right
/// is the difference between a triage queue that sorts itself and one that a
/// human has to read line by line.
///
/// Classification runs in two passes:
///   1. from the exception type, which is authoritative when TestForge threw it
///      (every TestForgeError carries its own category);
///   2. a refinement pass over the metadata the test recorded, which can
///      promote a generic failure into a specific one — an assertion that
///      failed against an HTTP 503 is really an application failure.
class FailureClassifier {
 public:
    struct Classification {
        TestStatus status = TestStatus::Error;
        FailureCategory category = FailureCategory::Unknown;
        std::string message;  ///< one line
        std::string detail;   ///< multi-line, may be empty
    };

    /// Classifies a caught exception.
    static Classification fromException(const std::exception& error);

    /// Classifies a `catch (...)` — no type information available.
    static Classification fromUnknownException();

    /// Second pass. `metadata` is TestContext-collected facts such as
    /// http_status, connect_error or exit_code. Returns a category that is
    /// never less specific than `base`.
    static FailureCategory refine(FailureCategory base, const json::Value& metadata);

    /// Heuristic classification from free text. Used when the only evidence is
    /// a message from a library that does not use TestForge's exceptions.
    static FailureCategory fromMessage(std::string_view message);

    /// One sentence explaining what a category means and who should act on it.
    /// Shown in reports and `testforge --help`.
    static std::string_view explain(FailureCategory category);

    /// Whether a failure in this category is worth retrying automatically.
    /// Assertion failures are not: a deterministic bug does not un-break
    /// itself, and retrying it just hides the signal.
    static bool isRetryable(FailureCategory category);
};

}  // namespace testforge
