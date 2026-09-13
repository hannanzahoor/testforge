#pragma once

#include "testforge/ai/AiProvider.hpp"
#include "testforge/core/Config.hpp"
#include "testforge/diagnostics/DiagnosticProvider.hpp"
#include "testforge/execution/TestRunner.hpp"
#include "testforge/persistence/ResultRepository.hpp"
#include "testforge/testing/TestRegistry.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace testforge::api {

/// Everything TestForge can do, as one object.
///
/// The CLI and the REST server are both thin shells over this class. That is
/// deliberate: it means `testforge run --suite smoke` and `POST /runs` take
/// exactly the same code path, so the two interfaces cannot drift, and the
/// interesting logic is testable without spawning a process or opening a
/// socket.
class TestForgeService {
 public:
    explicit TestForgeService(Config config);

    ~TestForgeService();

    TestForgeService(const TestForgeService&) = delete;
    TestForgeService& operator=(const TestForgeService&) = delete;
    TestForgeService(TestForgeService&&) = delete;
    TestForgeService& operator=(TestForgeService&&) = delete;

    /// Opens the database, installs diagnostics and the AI provider.
    /// Safe to call more than once. Throws ConfigurationError on a fatal
    /// misconfiguration; a database that cannot be opened is downgraded to a
    /// warning and persistence is disabled for the session.
    void initialise();

    [[nodiscard]] const Config& config() const noexcept { return config_; }

    [[nodiscard]] TestRegistry& registry() const noexcept { return *registry_; }

    [[nodiscard]] ResultRepositoryPtr repository() const noexcept { return repository_; }

    [[nodiscard]] std::shared_ptr<diagnostics::DiagnosticCollector> diagnostics() const noexcept {
        return diagnostics_;
    }

    [[nodiscard]] ai::AiProviderPtr aiProvider() const noexcept { return ai_; }

    void setAiProvider(ai::AiProviderPtr provider);

    void setDiagnosticCollector(std::shared_ptr<diagnostics::DiagnosticCollector> collector);

    // --- catalogue --------------------------------------------------------

    [[nodiscard]] std::vector<TestMetadata> listTests(const TestFilter& filter) const;

    [[nodiscard]] json::Value listTestsJson(const TestFilter& filter) const;

    [[nodiscard]] json::Value suitesJson() const;

    // --- execution --------------------------------------------------------

    /// Runs tests and writes the configured reports.
    ///
    /// `observers` receive live progress; the CLI passes its console reporter.
    TestRun run(const RunOptions& options,
                const std::vector<std::shared_ptr<RunObserver>>& observers = {});

    /// Runs are serialised: two concurrent runs sharing one registry and one
    /// results file would interleave in ways nobody wants to debug.
    [[nodiscard]] bool runInProgress() const;

    // --- history and analytics -------------------------------------------

    [[nodiscard]] json::Value historyJson(int limit) const;

    [[nodiscard]] json::Value statsJson(int runLimit) const;

    [[nodiscard]] json::Value runJson(const std::string& runId) const;

    [[nodiscard]] json::Value runResultsJson(const std::string& runId) const;

    [[nodiscard]] json::Value testHistoryJson(const std::string& testId, int limit) const;

    [[nodiscard]] json::Value flakyCandidatesJson(int limit) const;

    // --- diagnostics ------------------------------------------------------

    [[nodiscard]] json::Value diagnosticsJson() const;

    [[nodiscard]] json::Value gpuJson() const;

    [[nodiscard]] json::Value healthJson() const;

    // --- AI ---------------------------------------------------------------

    /// Generates, validates and (optionally) registers a suite.
    ///
    /// The returned object always carries the validation report, whether or
    /// not anything was registered — a rejection has to be visible.
    [[nodiscard]] json::Value generateTests(const std::string& requirement,
                                            const std::string& suiteName,
                                            int maxTests,
                                            bool registerSuite);

    /// Analyses one failed result from a stored run.
    [[nodiscard]] json::Value analyseFailure(const std::string& runId, const std::string& testName);

    /// Analyses a result supplied directly, without touching the database.
    [[nodiscard]] json::Value analyseResult(const TestResult& result);

    /// Loads a specification from a JSON file, validates it and registers it.
    [[nodiscard]] json::Value loadSpecFile(const std::string& path, bool registerSuite);

    /// Asks an in-flight run() to stop early. Safe to call from any thread,
    /// including while run() is executing on another one. Does nothing if no
    /// run is in progress.
    ///
    /// The run still completes normally: already-finished results are kept,
    /// the report is written, and the run is persisted. Cancellation is a
    /// request to stop starting new work, not an abort.
    void requestCancellation(const std::string& reason);

 private:
    [[nodiscard]] json::Value requireRepository() const;

    Config config_;
    TestRegistry* registry_;
    ResultRepositoryPtr repository_;
    std::shared_ptr<diagnostics::DiagnosticCollector> diagnostics_;
    ai::AiProviderPtr ai_;

    mutable std::mutex runMutex_;
    std::atomic<bool> running_{false};

    /// The runner executing right now, or nullptr. Guarded by its own mutex
    /// rather than runMutex_: the thread that wants to cancel cannot take
    /// runMutex_, because the thread it is trying to interrupt is holding it
    /// for the whole duration of the run.
    mutable std::mutex cancelMutex_;
    TestRunner* activeRunner_ = nullptr;
    bool initialised_ = false;
};

}  // namespace testforge::api
