#pragma once

#include "testforge/core/Json.hpp"
#include "testforge/core/TestResult.hpp"

#include <string>

namespace testforge::ai {

/// Builds the evidence package sent to a language model when a test fails.
///
/// Three concerns, all of which have to be handled before anything leaves the
/// process:
///
///   * **Relevance.** A model reasons better over a small, structured
///     document than over a dump. Only fields that bear on the failure go in.
///   * **Secrecy.** Everything passes through redaction, environment variables
///     are limited to a curated list, and request headers are dropped rather
///     than filtered.
///   * **Size.** Log tails, response bodies and diagnostics are truncated to
///     fixed budgets, so one enormous failure cannot produce an enormous bill.
///
/// The result is data, never instructions: the sidecar places it in a user
/// message and the system prompt tells the model that anything inside it is
/// untrusted content. See docs/security.md on prompt injection.
class FailureContext;

/// Size budgets. At namespace scope for the same reason as elsewhere in this
/// codebase: a nested type's default member initialisers are not available to
/// a default argument on the enclosing class.
struct FailureContextLimits {
    std::size_t maxLogLines = 40;
    std::size_t maxLogLineLength = 300;
    std::size_t maxErrorDetail = 4000;
    std::size_t maxResponseExcerpt = 1500;
    std::size_t maxTotalBytes = 24 * 1024;
};

class FailureContext {
 public:
    using Limits = FailureContextLimits;

    explicit FailureContext(FailureContextLimits limits = {}) : limits_(limits) {}

    /// Builds the context document for one failing result.
    [[nodiscard]] json::Value build(const TestResult& result) const;

    /// Adds run-level context (host, commit, worker count) to a document
    /// produced by build().
    [[nodiscard]] json::Value withRunContext(json::Value context, const TestRun& run) const;

    /// Adds recent history for the same test, which is what lets a model tell
    /// "newly broken" from "always broken" from "flaky".
    [[nodiscard]] json::Value withHistory(json::Value context, const json::Value& history) const;

 private:
    FailureContextLimits limits_;
};

}  // namespace testforge::ai
