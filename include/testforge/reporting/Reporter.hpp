#pragma once

#include "testforge/core/Config.hpp"
#include "testforge/core/TestResult.hpp"
#include "testforge/execution/TestRunner.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

namespace testforge::reporting {

/// Turns a completed run into some artefact.
///
/// Reporters are pure functions of a TestRun: they never query the database
/// and never touch the network, which makes every one of them trivially
/// testable against a hand-built TestRun.
class Reporter {
 public:
    virtual ~Reporter() = default;

    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;
    Reporter(Reporter&&) = delete;
    Reporter& operator=(Reporter&&) = delete;

    [[nodiscard]] virtual std::string_view name() const = 0;

    /// File extension this reporter produces, without the dot.
    [[nodiscard]] virtual std::string_view fileExtension() const = 0;

    [[nodiscard]] virtual std::string render(const TestRun& run) const = 0;

 protected:
    Reporter() = default;
};

/// Options for ConsoleReporter.
///
/// Declared at namespace scope: a nested class's default member initialisers
/// are not parsed until the end of the enclosing class, so `Options = {}` on a
/// constructor of that same class does not compile.
struct ConsoleReporterOptions {
    bool color = true;
    bool verboseFailures = true;
    bool showSkipReasons = true;
    bool live = true;  ///< stream per-test lines as they complete
    bool showDiagnostics = true;
    int maxFailuresShown = 25;
    std::ostream* stream = nullptr;  ///< defaults to std::cout
};

/// Human-readable terminal output.
///
/// Doubles as a RunObserver so results stream as they finish rather than
/// appearing in a block at the end — on a slow suite that difference is the
/// whole user experience.
class ConsoleReporter final : public Reporter, public RunObserver {
 public:
    using Options = ConsoleReporterOptions;

    explicit ConsoleReporter(ConsoleReporterOptions options = {});

    [[nodiscard]] std::string_view name() const override { return "console"; }

    [[nodiscard]] std::string_view fileExtension() const override { return "txt"; }

    [[nodiscard]] std::string render(const TestRun& run) const override;

    // --- RunObserver ------------------------------------------------------
    void onRunStarted(const TestRun& run, const std::vector<TestMetadata>& selected) override;
    void onTestFinished(const TestResult& result) override;
    void onRunFinished(const TestRun& run) override;

    /// Writes the full report to the configured stream.
    void print(const TestRun& run) const;

 private:
    [[nodiscard]] std::ostream& out() const;

    ConsoleReporterOptions options_;
    mutable std::mutex mutex_;  ///< onTestFinished is called from workers
    std::atomic<int> completed_{0};
    int selectedCount_ = 0;
};

/// Machine-readable output: the full TestRun as JSON.
class JsonReporter final : public Reporter {
 public:
    explicit JsonReporter(bool pretty = true) : pretty_(pretty) {}

    [[nodiscard]] std::string_view name() const override { return "json"; }

    [[nodiscard]] std::string_view fileExtension() const override { return "json"; }

    [[nodiscard]] std::string render(const TestRun& run) const override;

 private:
    bool pretty_;
};

/// Options for HtmlReporter.
struct HtmlReporterOptions {
    std::string title = "TestForge Report";
    bool includeLogs = true;
    bool includeDiagnostics = true;
    int maxLogLines = 60;
};

/// A single self-contained HTML file: no external CSS, no JavaScript
/// dependencies, nothing to serve. It opens from a file:// URL and survives
/// being emailed or attached to a CI job.
class HtmlReporter final : public Reporter {
 public:
    using Options = HtmlReporterOptions;

    explicit HtmlReporter(HtmlReporterOptions options = {}) : options_(std::move(options)) {}

    [[nodiscard]] std::string_view name() const override { return "html"; }

    [[nodiscard]] std::string_view fileExtension() const override { return "html"; }

    [[nodiscard]] std::string render(const TestRun& run) const override;

 private:
    HtmlReporterOptions options_;
};

/// JUnit XML, the format every CI system already understands. This is what
/// makes failures show up natively in a GitHub Actions or Jenkins summary.
class JUnitReporter final : public Reporter {
 public:
    [[nodiscard]] std::string_view name() const override { return "junit"; }

    [[nodiscard]] std::string_view fileExtension() const override { return "xml"; }

    [[nodiscard]] std::string render(const TestRun& run) const override;
};

/// Writes every enabled report to the configured output directory.
class ReportWriter {
 public:
    explicit ReportWriter(ReportingConfig config);

    /// Adds a reporter beyond the ones the configuration turns on.
    void addReporter(std::shared_ptr<Reporter> reporter);

    /// Writes the reports and returns the paths produced. Failures to write
    /// are logged and skipped, never thrown: losing a report must not turn a
    /// green run red.
    std::vector<std::string> write(const TestRun& run) const;

    /// Ensures the output directory exists. Returns false if it cannot.
    [[nodiscard]] bool prepareDirectory() const;

    /// "reports/run-9f2c1a-2026-09-12T063132Z.html"
    [[nodiscard]] std::string pathFor(const TestRun& run, const Reporter& reporter) const;

 private:
    ReportingConfig config_;
    std::vector<std::shared_ptr<Reporter>> extra_;
};

}  // namespace testforge::reporting
