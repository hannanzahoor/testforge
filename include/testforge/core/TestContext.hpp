#pragma once

#include "testforge/core/CancellationToken.hpp"
#include "testforge/core/Config.hpp"
#include "testforge/core/Json.hpp"
#include "testforge/core/Logger.hpp"
#include "testforge/core/TestCase.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace testforge {

/// Everything a test is allowed to reach for, handed to it explicitly.
///
/// A TestContext belongs to exactly one execution of one test on one thread.
/// It is never shared between workers, which is what makes it safe for it to
/// hold non-atomic mutable state (metadata, attachments).
class TestContext {
 public:
    TestContext(const Config& config,
                const TestMetadata& metadata,
                CancellationToken cancellation,
                std::string runId,
                std::string worker);

    [[nodiscard]] const Config& config() const noexcept { return *config_; }

    [[nodiscard]] const TestMetadata& metadata() const noexcept { return *metadata_; }

    [[nodiscard]] const std::string& runId() const noexcept { return runId_; }

    [[nodiscard]] const std::string& worker() const noexcept { return worker_; }

    [[nodiscard]] const Logger& log() const noexcept { return logger_; }

    [[nodiscard]] const CancellationToken& cancellation() const noexcept { return cancellation_; }

    /// Throws TestCancelled when the run has been cancelled or the deadline
    /// has passed. Call between logical steps of a long test.
    void throwIfCancelled() const;

    /// Cancellation-aware sleep. Returns false when cancelled early.
    bool sleepFor(Milliseconds duration) const { return cancellation_.waitFor(duration); }

    /// Time left before this test's deadline; Milliseconds::max() if unbounded.
    [[nodiscard]] Milliseconds remaining() const { return cancellation_.remaining(); }

    // --- facts recorded onto the TestResult ---------------------------------

    /// Attaches a fact to the result. Use this for anything a human would want
    /// when triaging: the URL called, the status code, the GPU name.
    void addMetadata(std::string key, json::Value value);

    void addTag(std::string tag);

    /// Free-form note appended to the result's error detail on failure. Use it
    /// to record what the test was doing when it failed.
    void addNote(std::string note);

    [[nodiscard]] const json::Value& collectedMetadata() const noexcept { return metadata_json_; }

    [[nodiscard]] const std::vector<std::string>& extraTags() const noexcept { return tags_; }

    [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }

    /// Marks the test skipped and unwinds by throwing SkipTest. Preferred over
    /// silently returning, so the result reflects reality.
    [[noreturn]] void skip(std::string reason) const;

    /// Reads a value from config.custom by dotted path.
    [[nodiscard]] const json::Value* configValue(std::string_view dottedPath) const;

    /// Effective timeout for this test after applying metadata and config.
    [[nodiscard]] Milliseconds effectiveTimeout() const;

 private:
    const Config* config_;
    const TestMetadata* metadata_;
    CancellationToken cancellation_;
    std::string runId_;
    std::string worker_;
    Logger logger_;

    json::Value metadata_json_ = json::Value::object();
    std::vector<std::string> tags_;
    std::vector<std::string> notes_;
};

}  // namespace testforge
