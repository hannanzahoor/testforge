#include "testforge/execution/TestRunner.hpp"

#include "testforge/core/CancellationToken.hpp"
#include "testforge/core/Environment.hpp"
#include "testforge/core/Exceptions.hpp"
#include "testforge/core/Ids.hpp"
#include "testforge/core/Logger.hpp"
#include "testforge/core/Process.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/execution/FailureClassifier.hpp"
#include "testforge/execution/ThreadPool.hpp"
#include "testforge/testing/Assertions.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <exception>
#include <future>
#include <sstream>
#include <thread>
#include <utility>

namespace testforge {
namespace {

/// Holds futures for tests that blew their deadline and did not stop.
///
/// The answer to "what happens to a test that ignores cancellation?". The
/// thread is not killed — that would leave locks held
/// and destructors unrun in a process whose job is to report trustworthy
/// results. Instead the runner stops waiting, records TIMEOUT, detaches the
/// thread, and parks its completion future here. At shutdown we give the
/// strays a bounded grace period; any still running are reported by name so
/// the author can fix the test.
///
/// The completion signal comes from a std::promise rather than std::async
/// precisely so that nothing has to be leaked: a future obtained from
/// std::async blocks in its own destructor until the task finishes, which
/// would hang shutdown on exactly the test that is already misbehaving. A
/// promise-backed future has no such destructor, and a detached thread's
/// stack is reclaimed by the runtime when it eventually exits.
///
/// docs/concurrency.md#orphaned-tests explains the trade-off in full.
class OrphanReaper {
 public:
    void adopt(std::string testName, std::future<void> finished) {
        const std::lock_guard<std::mutex> lock(mutex_);
        orphans_.push_back({std::move(testName), std::move(finished)});
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return orphans_.size();
    }

    /// Waits up to `grace` in total. Returns the names still running.
    std::vector<std::string> drain(Milliseconds grace) {
        std::vector<Orphan> pending;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            pending = std::move(orphans_);
            orphans_.clear();
        }

        const auto deadline = SteadyClock::now() + grace;
        std::vector<std::string> stuck;
        for (Orphan& orphan : pending) {
            const auto now = SteadyClock::now();
            const auto remaining = now >= deadline
                                       ? Milliseconds{0}
                                       : std::chrono::duration_cast<Milliseconds>(deadline - now);
            if (orphan.finished.valid() &&
                orphan.finished.wait_for(remaining) == std::future_status::ready) {
                // The stray thread completed after all. Nothing to report.
                continue;
            }
            stuck.push_back(orphan.testName);
        }
        // Nothing is leaked here. The futures come from a std::promise, whose
        // destructor does not block (unlike one from std::async), and the
        // threads themselves were detached at the point of abandonment.
        return stuck;
    }

 private:
    struct Orphan {
        std::string testName;
        std::future<void> finished;
    };

    mutable std::mutex mutex_;
    std::vector<Orphan> orphans_;
};

/// Everything one test execution needs, in one heap-allocated bundle.
///
/// This exists because of the timeout path. When a test blows its deadline the
/// worker stops waiting, but the test's thread is still running and still
/// touching its test object, its context and its result. If those lived on the
/// worker's stack the abandoned thread would write into freed memory the
/// moment runOne() returned. Keeping them in a shared_ptr that the background
/// thread also holds means the stray thread writes into memory it legitimately
/// owns, and the bundle is freed when the last of the two lets go.
///
/// Everything the TestContext points at must live here for the same reason.
/// TestContext holds non-owning pointers to its Config and TestMetadata, so an
/// orphan that outlives the reaper's grace period would otherwise read through
/// a dangling pointer the moment its owner went away — the Config in
/// particular belongs to the caller of TestRunner, not to the runner.
struct BodyExecution {
    Config config;          ///< owned copy: TestContext holds a pointer to it
    TestMetadata metadata;  ///< owned copy: the caller's may not outlive us
    TestCasePtr test;
    std::unique_ptr<TestContext> context;
    TestResult result;
    std::vector<std::string> logs;
};

/// Runs setUp / execute / tearDown, translating any exception into a verdict.
///
/// A free function rather than a member so that an orphaned thread never holds
/// a pointer back into TestRunner, which may be destroyed while it runs.
void executeBody(BodyExecution& execution) {
    // Captured on this thread, which is the thread the test body runs on —
    // that is what gives each test its own uninterleaved log tail.
    ScopedLogCapture capture(200);

    TestCase& test = *execution.test;
    TestContext& context = *execution.context;
    TestResult& result = execution.result;

    bool setUpSucceeded = false;
    try {
        test.setUp(context);
        setUpSucceeded = true;
        test.execute(context);
        result.status = TestStatus::Passed;
        result.failureCategory = FailureCategory::None;
    } catch (const std::exception& error) {
        const FailureClassifier::Classification classification =
            FailureClassifier::fromException(error);
        result.status = classification.status;
        result.failureCategory = classification.category;
        result.errorMessage = classification.message;
        result.errorDetail = classification.detail;

        // A failure inside setUp means the test never ran; say so plainly
        // rather than letting it look like an assertion in the body failed.
        if (!setUpSucceeded && result.status == TestStatus::Failed) {
            result.status = TestStatus::Error;
            result.errorDetail = "failure occurred during setUp\n" + result.errorDetail;
        }
    } catch (...) {
        const FailureClassifier::Classification classification =
            FailureClassifier::fromUnknownException();
        result.status = classification.status;
        result.failureCategory = classification.category;
        result.errorMessage = classification.message;
        result.errorDetail = classification.detail;
    }

    if (setUpSucceeded) {
        // tearDown is noexcept by contract, so it cannot mask the failure
        // recorded above.
        test.tearDown(context);
    }

    execution.logs = capture.lines();
}

/// What happened while waiting for a bounded test body.
struct BodyOutcome {
    bool completed = false;         ///< the body finished; the bundle is safe to read
    bool deadlineExceeded = false;  ///< the deadline passed before it finished
};

/// Runs a test body on a helper thread and waits for it, or for its deadline.
///
/// Split out of runOne because it is the one part with genuinely intricate
/// control flow: three ways to leave the loop, a grace period, and two very
/// different dispositions for the thread depending on which happened.
///
/// std::thread with an explicit promise rather than std::async: an async
/// future blocks in its destructor until the task completes, so abandoning one
/// would either hang shutdown or have to be leaked. Here the thread is
/// detached when abandoned and the promise-backed future is free to be
/// destroyed at any time.
///
/// Both the lambda and the caller hold the shared bundle, so a stray thread
/// writes into memory it legitimately owns.
BodyOutcome awaitBoundedBody(const std::shared_ptr<BodyExecution>& execution,
                             CancellationSource& testCancellation,
                             const CancellationSource& runCancellation,
                             std::int64_t timeoutMs,
                             std::int64_t graceMs,
                             const std::string& qualifiedName,
                             OrphanReaper& reaper) {
    BodyOutcome outcome;

    auto finishedPromise = std::make_shared<std::promise<void>>();
    std::future<void> body = finishedPromise->get_future();
    std::thread bodyThread([execution, finishedPromise] {
        executeBody(*execution);
        finishedPromise->set_value();
    });

    // Poll in slices rather than one long wait so a run-wide cancellation is
    // forwarded to this test promptly.
    const auto deadline = SteadyClock::now() + Milliseconds{timeoutMs};
    while (true) {
        const auto now = SteadyClock::now();
        if (now >= deadline) {
            break;
        }
        const auto slice = std::min<Milliseconds>(
            Milliseconds{25}, std::chrono::duration_cast<Milliseconds>(deadline - now));
        if (body.wait_for(slice) == std::future_status::ready) {
            outcome.completed = true;
            break;
        }
        // requestCancellation() signals this test's source
        // directly, but fail-fast cancels only the run source, and a source
        // registered a moment too late would otherwise be missed.
        if (runCancellation.isCancelled() && !testCancellation.isCancelled()) {
            testCancellation.cancel(runCancellation.token().reason());
        }
    }

    if (!outcome.completed) {
        // Deadline reached. Ask the test to stop, then allow a short grace
        // period for it to unwind before giving up on the thread.
        outcome.deadlineExceeded = true;
        testCancellation.cancel("deadline of " + std::to_string(timeoutMs) + "ms exceeded");
        if (body.wait_for(Milliseconds{graceMs}) == std::future_status::ready) {
            outcome.completed = true;
        }
    }

    if (outcome.completed) {
        bodyThread.join();
    } else {
        // Detached, never killed. The reaper keeps the completion future so
        // shutdown can wait a bounded grace period and name whatever is still
        // running. See docs/concurrency.md#orphaned-tests.
        bodyThread.detach();
        reaper.adopt(qualifiedName, std::move(body));
    }

    return outcome;
}

/// The verdict for a test that blew its deadline and is still running.
///
/// Nothing in the bundle may be read here: the stray thread is still writing
/// to it. Everything below is known from outside.
void describeAbandonedTest(TestResult& result, std::int64_t timeoutMs, int attempt) {
    result.status = TestStatus::Timeout;
    result.failureCategory = FailureCategory::Timeout;
    result.errorMessage = "test exceeded its " + std::to_string(timeoutMs) +
                          "ms deadline and did not respond to cancellation";
    result.errorDetail =
        "The test body is still running on a background thread. TestForge never forcibly "
        "terminates a thread (see docs/concurrency.md), so the thread is left to finish and "
        "is reported again at shutdown. Make the test cooperative by calling "
        "ctx.throwIfCancelled() inside its loops, or ctx.sleepFor() instead of "
        "std::this_thread::sleep_for.";
    result.metadata.set("timeout_ms", timeoutMs);
    result.metadata.set("attempt", attempt);
    result.metadata.set("deadline_exceeded", true);
    result.metadata.set("abandoned_thread", true);
}

/// Overrides the body's own verdict when the deadline or a run-wide
/// cancellation means it cannot be trusted.
void overrideVerdictForCancellation(TestResult& result,
                                    bool deadlineExceeded,
                                    bool cancelledByRun,
                                    std::int64_t timeoutMs,
                                    const std::string& runCancellationReason) {
    if (deadlineExceeded) {
        // The body did eventually finish, but only inside the grace period we
        // granted after cancelling it. Whatever verdict it reached is not
        // trustworthy — it was racing a deadline it had already lost — so the
        // recorded outcome is TIMEOUT either way.
        const bool unwoundCleanly = result.status == TestStatus::Timeout;
        result.status = TestStatus::Timeout;
        result.failureCategory = FailureCategory::Timeout;
        if (!unwoundCleanly) {
            result.errorMessage = "test exceeded its " + std::to_string(timeoutMs) +
                                  "ms deadline and only finished during the cancellation grace "
                                  "period";
            result.errorDetail =
                "The test did not observe cancellation and ran to completion after the deadline "
                "had already passed. Add a ctx.throwIfCancelled() checkpoint, or use "
                "ctx.sleepFor() instead of std::this_thread::sleep_for, so it can stop when "
                "asked. See docs/concurrency.md.";
        }
        result.metadata.set("deadline_exceeded", true);
        result.metadata.set("finished_in_grace_period", true);
    } else if (cancelledByRun) {
        // The run was cancelled underneath this test — Ctrl-C, SIGTERM, or
        // fail-fast. Whatever the body concluded after that point is an
        // artefact of the cancellation, not a property of the code: a
        // cancellation-aware sleep returns early, an HTTP call is abandoned,
        // and the assertions that follow fail for reasons the author never
        // wrote a bug for.
        //
        // Reporting that as FAILED would tell a user who just pressed Ctrl-C
        // that their tests are broken. Skipped is the accurate verdict and it
        // keeps the run's exit code meaningful.
        result.status = TestStatus::Skipped;
        result.failureCategory = FailureCategory::None;
        result.errorMessage = "cancelled: " + runCancellationReason;
        result.errorDetail =
            "The run was cancelled while this test was executing, so its outcome was discarded "
            "rather than recorded as a pass or a failure.";
        result.metadata.set("run_cancelled", true);
    }
}

/// Folds everything the test recorded through its context into the result.
void applyContextOutput(const TestContext& context, TestResult& result) {
    for (const json::Member& member : context.collectedMetadata().asObject()) {
        result.metadata.set(member.first, member.second);
    }
    for (const std::string& tag : context.extraTags()) {
        if (std::find(result.tags.begin(), result.tags.end(), tag) == result.tags.end()) {
            result.tags.push_back(tag);
        }
    }
    if (!context.notes().empty()) {
        std::ostringstream notes;
        for (const std::string& note : context.notes()) {
            notes << "\n  note: " << note;
        }
        result.errorDetail += notes.str();
    }
}

std::string readHostname() {
    const ProcessResult result = ProcessRunner::run("uname", {"-n"}, ProcessOptions{});
    if (result.ok()) {
        return strings::trim(result.standardOutput);
    }
    return env::getOr("HOSTNAME", "unknown");
}

std::string readGitCommit() {
    ProcessOptions options;
    options.timeout = Milliseconds{2'000};
    const ProcessResult result =
        ProcessRunner::run("git", {"rev-parse", "--short", "HEAD"}, options);
    if (result.ok()) {
        return strings::trim(result.standardOutput);
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct TestRunner::Impl {
    Impl(const Config& configuration, TestRegistry& testRegistry)
        : config(configuration), registry(testRegistry), logger("runner") {}

    const Config& config;
    TestRegistry& registry;
    Logger logger;

    ResultRepositoryPtr repository;
    std::shared_ptr<diagnostics::DiagnosticCollector> diagnosticCollector;
    std::vector<std::shared_ptr<RunObserver>> observers;

    /// Cancellation for the run as a whole (Ctrl-C, fail-fast).
    std::unique_ptr<CancellationSource> runCancellation = std::make_unique<CancellationSource>();
    std::atomic<bool> cancelRequested{false};

    /// Cancellation sources for the tests executing right now.
    ///
    /// The run-level source alone is not enough. A test with no timeout runs
    /// inline on the worker thread, so nothing is polling the run source on
    /// its behalf and it would never observe a Ctrl-C at all. Registering each
    /// active test here lets requestCancellation() reach every one of them
    /// directly, which also removes the up-to-25ms forwarding lag on the timed
    /// path.
    ///
    /// Entries are raw pointers to stack objects in runOne(). That is safe
    /// because registration is scoped by ActiveRegistration, whose destructor
    /// runs *before* the source it refers to and takes the same mutex that
    /// cancelAllActive() holds — so a pointer is either still registered and
    /// its object still alive, or already gone from the vector.
    std::mutex activeMutex;
    std::vector<CancellationSource*> activeTests;

    void registerActive(CancellationSource* source) {
        const std::lock_guard<std::mutex> lock(activeMutex);
        activeTests.push_back(source);
    }

    void unregisterActive(CancellationSource* source) {
        const std::lock_guard<std::mutex> lock(activeMutex);
        activeTests.erase(std::remove(activeTests.begin(), activeTests.end(), source),
                          activeTests.end());
    }

    void cancelAllActive(const std::string& reason) {
        const std::lock_guard<std::mutex> lock(activeMutex);
        for (CancellationSource* source : activeTests) {
            source->cancel(reason);
        }
    }

    OrphanReaper reaper;

    std::mutex resultsMutex;

    void notifyStarted(const TestRun& run, const std::vector<TestMetadata>& selected) {
        for (const auto& observer : observers) {
            observer->onRunStarted(run, selected);
        }
    }

    void notifyTestStarted(const TestMetadata& test) {
        for (const auto& observer : observers) {
            observer->onTestStarted(test);
        }
    }

    void notifyTestFinished(const TestResult& result) {
        for (const auto& observer : observers) {
            observer->onTestFinished(result);
        }
    }

    void notifyFinished(const TestRun& run) {
        for (const auto& observer : observers) {
            observer->onRunFinished(run);
        }
    }
};

// ---------------------------------------------------------------------------
// TestRunner
// ---------------------------------------------------------------------------

TestRunner::TestRunner(const Config& config, TestRegistry& registry)
    : impl_(std::make_unique<Impl>(config, registry)) {}

TestRunner::~TestRunner() {
    const std::vector<std::string> stuck =
        impl_->reaper.drain(Milliseconds{impl_->config.execution.cancellationGraceMs});
    if (!stuck.empty()) {
        impl_->logger.warn("tests ignored cancellation and are still running at shutdown",
                           {{"count", static_cast<std::int64_t>(stuck.size())},
                            {"tests", strings::join(stuck, ", ")}});
    }
}

void TestRunner::setRepository(ResultRepositoryPtr repository) {
    impl_->repository = std::move(repository);
}

void TestRunner::setDiagnosticCollector(
    std::shared_ptr<diagnostics::DiagnosticCollector> collector) {
    impl_->diagnosticCollector = std::move(collector);
}

void TestRunner::addObserver(std::shared_ptr<RunObserver> observer) {
    if (observer) {
        impl_->observers.push_back(std::move(observer));
    }
}

void TestRunner::requestCancellation(const std::string& reason) {
    impl_->cancelRequested.store(true, std::memory_order_release);
    impl_->runCancellation->cancel(reason);
    // Reach the tests that are already running. Without this a test with no
    // timeout never learns the run was cancelled, because nothing polls the
    // run source on its behalf.
    impl_->cancelAllActive(reason);
}

bool TestRunner::cancellationRequested() const noexcept {
    return impl_->cancelRequested.load(std::memory_order_acquire);
}

TestResult TestRunner::runOne(const TestMetadata& metadata, const std::string& runId, int attempt) {
    const std::string worker = ThreadPool::currentWorkerName();
    Stopwatch watch;

    TestResult result = TestResult::make(metadata.suite, metadata.name, TestStatus::Error);
    result.runId = runId;
    result.attempt = attempt;
    result.tags = metadata.tags;
    result.worker = worker;
    result.startTime = WallClock::now();

    auto finish = [&watch](TestResult& out) {
        out.endTime = WallClock::now();
        out.duration = watch.elapsed();
        return out;
    };

    if (!metadata.enabled) {
        result.status = TestStatus::Skipped;
        result.failureCategory = FailureCategory::None;
        result.errorMessage =
            metadata.disabledReason.empty() ? "test is disabled" : metadata.disabledReason;
        return finish(result);
    }

    TestCasePtr test = impl_->registry.create(metadata.qualifiedName());
    if (!test) {
        result.status = TestStatus::Error;
        result.failureCategory = FailureCategory::FrameworkError;
        result.errorMessage = "test is registered but its factory produced nothing";
        return finish(result);
    }

    const std::int64_t timeoutMs =
        metadata.timeoutMs > 0 ? metadata.timeoutMs : impl_->config.execution.defaultTimeoutMs;
    const bool bounded = timeoutMs > 0;

    // Per-test cancellation, chained to the run-wide source below so that a
    // Ctrl-C or a fail-fast trip reaches tests that are already running.
    CancellationSource testCancellation;
    if (bounded) {
        testCancellation.setDeadline(SteadyClock::now() + Milliseconds{timeoutMs});
    }

    // Publish this test's source for the duration of the execution, so that
    // requestCancellation() can reach it directly. Declared after
    // testCancellation so it is destroyed *before* it: the deregistration must
    // happen while the object it names is still alive.
    struct ActiveRegistration {
        Impl* impl;
        CancellationSource* source;

        ActiveRegistration(Impl* owner, CancellationSource* cancellation)
            : impl(owner), source(cancellation) {
            impl->registerActive(source);
        }

        ~ActiveRegistration() { impl->unregisterActive(source); }

        ActiveRegistration(const ActiveRegistration&) = delete;
        ActiveRegistration& operator=(const ActiveRegistration&) = delete;
        ActiveRegistration(ActiveRegistration&&) = delete;
        ActiveRegistration& operator=(ActiveRegistration&&) = delete;
    } activeRegistration{impl_.get(), &testCancellation};

    bool deadlineExceeded = false;

    if (impl_->runCancellation->isCancelled()) {
        // The run was already going down before this test started. Running it
        // would only produce a verdict nobody should trust.
        result.status = TestStatus::Skipped;
        result.failureCategory = FailureCategory::None;
        result.errorMessage = "not run: " + impl_->runCancellation->token().reason();
        result.metadata.set("run_cancelled", true);
        return finish(result);
    }

    auto execution = std::make_shared<BodyExecution>();
    execution->config = impl_->config;  // copy: see BodyExecution
    execution->metadata = metadata;
    execution->test = std::move(test);
    execution->result = result;
    execution->context = std::make_unique<TestContext>(
        execution->config, execution->metadata, testCancellation.token(), runId, worker);

    // An unbounded test runs inline on this thread; a bounded one runs on a
    // helper thread so that this one can stop waiting at the deadline without
    // killing anything.
    bool completed = true;
    if (!bounded) {
        executeBody(*execution);
    } else {
        const BodyOutcome outcome = awaitBoundedBody(execution,
                                                     testCancellation,
                                                     *impl_->runCancellation,
                                                     timeoutMs,
                                                     impl_->config.execution.cancellationGraceMs,
                                                     metadata.qualifiedName(),
                                                     impl_->reaper);
        completed = outcome.completed;
        deadlineExceeded = outcome.deadlineExceeded;
    }

    if (!completed) {
        describeAbandonedTest(result, timeoutMs, attempt);
        return finish(result);
    }

    // Safe to read: the body thread has joined (or never existed).
    result = execution->result;
    result.logs = execution->logs;
    result.worker = worker;
    result.attempt = attempt;

    // Was this test stopped because the whole run was, rather than because it
    // blew its own deadline? Those deserve different verdicts: the first is
    // not the test's fault and must not be reported as a failure.
    //
    // Derived rather than flagged, so it is correct on both paths. A deadline
    // is the only other thing that cancels this source, and deadlineExceeded
    // records that; an unbounded test has no deadline at all, so for it any
    // cancellation is necessarily the run's.
    const bool cancelledByRun = testCancellation.isCancelled() && !deadlineExceeded;

    overrideVerdictForCancellation(result,
                                   deadlineExceeded,
                                   cancelledByRun,
                                   timeoutMs,
                                   impl_->runCancellation->token().reason());

    applyContextOutput(*execution->context, result);

    // Second classification pass, now that the test's own metadata is visible:
    // an assertion that failed against an HTTP 503 is an application failure.
    if (isFailure(result.status)) {
        result.failureCategory = FailureClassifier::refine(result.failureCategory, result.metadata);
    }

    result.metadata.set("timeout_ms", timeoutMs);
    result.metadata.set("attempt", attempt);

    return finish(result);
}

TestRun TestRunner::run(const RunOptions& options) {
    Impl& impl = *impl_;

    const std::vector<TestMetadata> selected =
        TestSelector::select(impl.registry, options.filter, options.selection);

    TestRun run;
    run.runId = ids::generateHexId(16);
    run.label = options.label;
    run.startedAt = WallClock::now();
    run.filterDescription = options.filter.describe();
    run.hostname = readHostname();
    run.gitCommit = readGitCommit();

    const int workers = options.workers > 0 ? options.workers : impl.config.effectiveWorkers();
    run.workers = std::max(1, workers);

    LogManager::instance().setRunId(run.runId);

    json::Value environment = json::Value::object();
    environment.set("hostname", run.hostname);
    environment.set("workers", run.workers);
    environment.set("config", impl.config.toJson());
    environment.set("variables", [] {
        json::Value out = json::Value::object();
        for (const auto& [key, value] : env::diagnosticSubset()) {
            out.set(key, value);
        }
        return out;
    }());
    run.environment = environment;

    impl.logger.info("run starting",
                     {{"run_id", run.runId},
                      {"tests", static_cast<std::int64_t>(selected.size())},
                      {"workers", run.workers},
                      {"filter", run.filterDescription}});

    if (options.persist && impl.repository) {
        try {
            impl.repository->beginRun(run);
        } catch (const std::exception& error) {
            // Persistence must never take down a run: the results are the
            // product, the database is a convenience.
            impl.logger.error("could not record run start", {{"error", std::string(error.what())}});
        }
    }

    impl.notifyStarted(run, selected);

    Stopwatch wallClock;
    std::vector<TestResult> results;
    results.reserve(selected.size());

    const bool collectDiagnostics =
        options.collectDiagnostics.value_or(impl.config.diagnostics.collectOnFailure);

    std::atomic<bool> stopScheduling{false};

    auto handleResult = [&](TestResult result) {
        if (isFailure(result.status) && collectDiagnostics && impl.diagnosticCollector) {
            result.diagnostics = impl.diagnosticCollector->collectForFailure();
        }

        {
            const std::lock_guard<std::mutex> lock(impl.resultsMutex);
            results.push_back(result);
        }

        if (options.persist && impl.repository) {
            try {
                impl.repository->saveResult(result);
            } catch (const std::exception& error) {
                impl.logger.error(
                    "could not persist result",
                    {{"test", result.qualifiedName()}, {"error", std::string(error.what())}});
            }
        }

        impl.notifyTestFinished(result);

        if (options.failFast && isFailure(result.status)) {
            stopScheduling.store(true, std::memory_order_release);
            impl.runCancellation->cancel("fail-fast: " + result.qualifiedName() + " failed");
        }
    };

    // Executes one test including the retry policy. Retries are handled here
    // rather than inside runOne so that each attempt produces its own result
    // and the history shows what actually happened.
    auto executeWithRetries = [&](const TestMetadata& metadata) {
        if (stopScheduling.load(std::memory_order_acquire)) {
            return;
        }
        impl.notifyTestStarted(metadata);

        const int maxAttempts = 1 + std::max(0, options.retryFailed);
        TestResult result;
        for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
            result = runOne(metadata, run.runId, attempt);
            if (!isFailure(result.status)) {
                if (attempt > 1) {
                    // Passed only after a retry: precisely the signature of a
                    // flaky test, so flag it for the analytics layer.
                    result.flakyCandidate = true;
                }
                break;
            }
            if (attempt < maxAttempts && !FailureClassifier::isRetryable(result.failureCategory)) {
                break;  // deterministic failure; retrying only wastes time
            }
            if (attempt < maxAttempts) {
                impl.logger.warn("retrying failed test",
                                 {{"test", metadata.qualifiedName()},
                                  {"attempt", attempt},
                                  {"category", std::string(toString(result.failureCategory))}});
                // Record the failed attempt too, so history is complete.
                if (options.persist && impl.repository) {
                    try {
                        impl.repository->saveResult(result);
                    } catch (const std::exception&) {
                        // already logged elsewhere; keep going
                    }
                }
            }
        }
        handleResult(std::move(result));
    };

    if (run.workers == 1 || selected.size() <= 1) {
        // Sequential path. Not just an optimisation: with one worker there is
        // no interleaving, which makes debugging a failing test far easier.
        for (const TestMetadata& metadata : selected) {
            executeWithRetries(metadata);
        }
    } else {
        ThreadPool pool(static_cast<std::size_t>(run.workers), "worker");
        std::vector<std::future<void>> pending;
        pending.reserve(selected.size());
        for (const TestMetadata& metadata : selected) {
            pending.push_back(
                pool.submit([&executeWithRetries, metadata] { executeWithRetries(metadata); }));
        }
        for (std::future<void>& future : pending) {
            try {
                future.get();
            } catch (const std::exception& error) {
                impl.logger.error("scheduling error", {{"error", std::string(error.what())}});
            }
        }
        pool.shutdown();
    }

    run.duration = wallClock.elapsed();
    run.finishedAt = WallClock::now();

    // Record that the run stopped early, so the exit code and the report can
    // say "incomplete" rather than "pass".
    if (impl.runCancellation->isCancelled()) {
        run.cancellationReason = impl.runCancellation->token().reason();
    }

    // Deterministic ordering for the report regardless of completion order.
    std::sort(results.begin(), results.end(), [](const TestResult& a, const TestResult& b) {
        return a.qualifiedName() < b.qualifiedName();
    });
    run.results = std::move(results);

    if (options.persist && impl.repository) {
        try {
            impl.repository->completeRun(run);
        } catch (const std::exception& error) {
            impl.logger.error("could not finalise run record",
                              {{"error", std::string(error.what())}});
        }
    }

    const RunStatistics stats = run.statistics();
    impl.logger.info("run finished",
                     {{"run_id", run.runId},
                      {"passed", stats.passed},
                      {"failed", stats.failed},
                      {"skipped", stats.skipped},
                      {"errors", stats.errors},
                      {"timeouts", stats.timeouts},
                      {"duration_ms", millisOf(run.duration)}});

    impl.notifyFinished(run);
    return run;
}

}  // namespace testforge
