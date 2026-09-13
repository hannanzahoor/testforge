#pragma once

#include "testforge/core/Config.hpp"
#include "testforge/core/TestResult.hpp"
#include "testforge/diagnostics/DiagnosticProvider.hpp"
#include "testforge/persistence/ResultRepository.hpp"
#include "testforge/testing/TestRegistry.hpp"
#include "testforge/testing/TestSelector.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace testforge {

/// Receives run progress as it happens.
///
/// The console reporter implements this so output streams live instead of
/// appearing all at once at the end; the REST API implements it to update a
/// run's status.
class RunObserver {
 public:
    virtual ~RunObserver() = default;

    RunObserver(const RunObserver&) = delete;
    RunObserver& operator=(const RunObserver&) = delete;
    RunObserver(RunObserver&&) = delete;
    RunObserver& operator=(RunObserver&&) = delete;

    virtual void onRunStarted(const TestRun& run, const std::vector<TestMetadata>& selected) {
        (void)run;
        (void)selected;
    }

    /// Called from a worker thread. Implementations must be thread-safe.
    virtual void onTestStarted(const TestMetadata& test) { (void)test; }

    /// Called from a worker thread. Implementations must be thread-safe.
    virtual void onTestFinished(const TestResult& result) { (void)result; }

    virtual void onRunFinished(const TestRun& run) { (void)run; }

 protected:
    RunObserver() = default;
};

/// Options for one invocation of the runner.
struct RunOptions {
    TestFilter filter;
    SelectionOptions selection;

    /// 0 = use Config::effectiveWorkers().
    int workers = 0;

    /// Overrides Config when > 0.
    std::int64_t timeoutMsOverride = 0;

    bool failFast = false;
    int retryFailed = 0;
    std::string label;

    /// Skip persistence for this run (used by benchmarks and dry runs).
    bool persist = true;

    /// Collect diagnostics on failure. Overrides the config default when set.
    std::optional<bool> collectDiagnostics;
};

/// Executes tests.
///
/// Responsibilities, in order:
///   discover -> select -> schedule -> execute -> classify -> collect
///   diagnostics -> persist -> notify observers.
///
/// The runner owns none of its collaborators: repository, diagnostics and
/// observers are injected, and every one of them is optional. A runner with no
/// repository and no diagnostics still runs tests correctly — which is exactly
/// how the unit tests exercise it.
class TestRunner {
 public:
    TestRunner(const Config& config, TestRegistry& registry);

    ~TestRunner();

    TestRunner(const TestRunner&) = delete;
    TestRunner& operator=(const TestRunner&) = delete;
    TestRunner(TestRunner&&) = delete;
    TestRunner& operator=(TestRunner&&) = delete;

    void setRepository(ResultRepositoryPtr repository);

    void setDiagnosticCollector(std::shared_ptr<diagnostics::DiagnosticCollector> collector);

    void addObserver(std::shared_ptr<RunObserver> observer);

    /// Runs everything matching `options`. Never throws for test failures;
    /// throws ConfigurationError only when the request itself is invalid.
    TestRun run(const RunOptions& options);

    /// Executes a single already-selected test, including setUp/tearDown,
    /// timeout enforcement, classification and diagnostics — but no
    /// persistence and no observers. Exposed because it is the natural unit to
    /// test, and the REST API uses it for one-off executions.
    TestResult runOne(const TestMetadata& metadata, const std::string& runId, int attempt = 1);

    /// Cancels an in-flight run. Safe to call from a signal handler context
    /// (it only sets an atomic and signals a condition variable).
    void requestCancellation(const std::string& reason);

    [[nodiscard]] bool cancellationRequested() const noexcept;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace testforge
