/// Tests for the concurrency primitives and the failure classifier.

#include "testforge/core/CancellationToken.hpp"
#include "testforge/core/Exceptions.hpp"
#include "testforge/execution/FailureClassifier.hpp"
#include "testforge/execution/ThreadPool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace testforge;

// ---------------------------------------------------------------------------
// ThreadPool
// ---------------------------------------------------------------------------

TEST(ThreadPool, RunsEveryTaskExactlyOnce) {
    constexpr int kTasks = 1000;
    std::atomic<int> executed{0};
    std::atomic<long long> sum{0};

    {
        ThreadPool pool(4);
        std::vector<std::future<void>> pending;
        pending.reserve(kTasks);
        for (int i = 0; i < kTasks; ++i) {
            pending.push_back(pool.submit([&executed, &sum, i] {
                executed.fetch_add(1, std::memory_order_relaxed);
                sum.fetch_add(i, std::memory_order_relaxed);
            }));
        }
        for (auto& future : pending) {
            future.get();
        }
    }

    EXPECT_EQ(executed.load(), kTasks);
    // The sum catches a task that ran twice, which a count alone would miss.
    EXPECT_EQ(sum.load(), static_cast<long long>(kTasks) * (kTasks - 1) / 2);
}

TEST(ThreadPool, ReturnsValuesThroughFutures) {
    ThreadPool pool(2);
    std::future<int> answer = pool.submit([] { return 42; });
    std::future<std::string> text =
        pool.submit([](std::string suffix) { return "a" + suffix; }, std::string("b"));
    EXPECT_EQ(answer.get(), 42);
    EXPECT_EQ(text.get(), "ab");
}

TEST(ThreadPool, ExceptionsTravelInTheFutureAndDoNotKillTheWorker) {
    ThreadPool pool(1);  // one worker, so a dead worker would deadlock the rest

    std::future<int> bad = pool.submit([]() -> int { throw std::runtime_error("boom"); });
    EXPECT_THROW((void)bad.get(), std::runtime_error);

    std::future<int> good = pool.submit([] { return 7; });
    EXPECT_EQ(good.get(), 7);
}

TEST(ThreadPool, UsesAllItsWorkers) {
    constexpr std::size_t kWorkers = 4;
    ThreadPool pool(kWorkers, "spread");

    std::mutex mutex;
    std::set<std::string> observed;
    std::atomic<int> arrived{0};

    // Each task blocks until all of them have arrived, which cannot happen
    // unless the tasks really are running concurrently.
    std::vector<std::future<void>> pending;
    for (std::size_t i = 0; i < kWorkers; ++i) {
        pending.push_back(pool.submit([&] {
            {
                const std::lock_guard<std::mutex> lock(mutex);
                observed.insert(ThreadPool::currentWorkerName());
            }
            arrived.fetch_add(1);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (arrived.load() < static_cast<int>(kWorkers) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }));
    }
    for (auto& future : pending) {
        future.get();
    }

    EXPECT_EQ(observed.size(), kWorkers);
    EXPECT_EQ(pool.workerCount(), kWorkers);
}

TEST(ThreadPool, WorkerNameIsMainOutsideThePool) {
    EXPECT_EQ(ThreadPool::currentWorkerName(), "main");
}

TEST(ThreadPool, WaitIdleDrainsTheQueue) {
    ThreadPool pool(2);
    std::atomic<int> done{0};
    for (int i = 0; i < 50; ++i) {
        (void)pool.submit([&done] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            done.fetch_add(1);
        });
    }
    pool.waitIdle();
    EXPECT_EQ(done.load(), 50);
    EXPECT_EQ(pool.queueDepth(), 0U);
}

TEST(ThreadPool, ShutdownDrainsQueuedWork) {
    std::atomic<int> done{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 20; ++i) {
            (void)pool.submit([&done] { done.fetch_add(1); });
        }
        pool.shutdown();
    }
    // The destructor defaults to draining: losing queued results silently
    // would be worse than waiting.
    EXPECT_EQ(done.load(), 20);
}

TEST(ThreadPool, SubmitAfterShutdownIsRejected) {
    ThreadPool pool(1);
    pool.shutdown();
    EXPECT_THROW((void)pool.submit([] {}), std::runtime_error);
}

TEST(ThreadPool, ZeroWorkersIsClampedToOne) {
    ThreadPool pool(0);
    EXPECT_EQ(pool.workerCount(), 1U);
    EXPECT_EQ(pool.submit([] { return 1; }).get(), 1);
}

// ---------------------------------------------------------------------------
// shutdownNow()
// ---------------------------------------------------------------------------
//
// The discard path is the subtlest code in the pool. It has to drop queued
// tasks *and* keep the pending count consistent, because pending_ is what
// waitIdle() blocks on -- an accounting slip there is a hang, not a wrong
// answer. None of it was covered before.

TEST(ThreadPool, ShutdownNowDiscardsQueuedWork) {
    std::atomic<int> executed{0};
    std::atomic<bool> workerBusy{false};
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();

    ThreadPool pool(1, "discard");

    // One worker, so everything after the blocker is stuck in the queue.
    auto blocking = pool.submit([gate, &executed, &workerBusy] {
        workerBusy.store(true, std::memory_order_release);
        gate.wait();
        executed.fetch_add(1, std::memory_order_relaxed);
    });
    for (int i = 0; i < 20; ++i) {
        (void)pool.submit([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
    }

    while (!workerBusy.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // shutdownNow() from another thread: it publishes the discard flags and
    // then blocks joining the worker, which is still inside the blocking task.
    std::thread stopper([&pool] { pool.shutdownNow(); });

    // Ordering, not timing. Once stopping() is observable the discard flag is
    // set too -- both are written under the same lock before the join -- so
    // releasing the worker now guarantees it sees them before it can take
    // another task. Releasing earlier is what made this race.
    while (!pool.stopping()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    release.set_value();
    stopper.join();
    blocking.wait();

    EXPECT_EQ(executed.load(), 1)
        << "only the task already running should have executed; the queue was to be discarded";
}

TEST(ThreadPool, ShutdownNowWaitsForWorkAlreadyRunning) {
    // Discarding the queue must not mean abandoning a task mid-flight. The
    // pool joins its workers, so a running task always finishes first.
    std::atomic<bool> finished{false};

    ThreadPool pool(1, "inflight");
    (void)pool.submit([&finished] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        finished.store(true, std::memory_order_release);
    });

    // Give the worker time to pick it up, so this is genuinely "in flight".
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    pool.shutdownNow();

    EXPECT_TRUE(finished.load(std::memory_order_acquire))
        << "shutdownNow returned while a task was still running";
}

TEST(ThreadPool, WaitIdleDoesNotHangAfterShutdownNow) {
    // The accounting test, stated as the symptom it prevents. If the discard
    // path forgot to decrement pending_ for a dropped task, this blocks
    // forever -- so it is run on another thread with a deadline rather than
    // being allowed to wedge the suite.
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();

    ThreadPool pool(1, "accounting");
    (void)pool.submit([gate] { gate.wait(); });
    for (int i = 0; i < 50; ++i) {
        (void)pool.submit([] {});
    }

    release.set_value();
    pool.shutdownNow();

    std::promise<void> returned;
    std::future<void> signal = returned.get_future();
    std::thread waiter([&pool, &returned] {
        pool.waitIdle();
        returned.set_value();
    });

    EXPECT_EQ(signal.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "waitIdle() hung: pending_ was not decremented for discarded tasks";
    waiter.join();
}

TEST(ThreadPool, QueueIsEmptyAfterShutdownNow) {
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();

    ThreadPool pool(1, "drained");
    (void)pool.submit([gate] { gate.wait(); });
    for (int i = 0; i < 10; ++i) {
        (void)pool.submit([] {});
    }
    release.set_value();
    pool.shutdownNow();

    EXPECT_EQ(pool.queueDepth(), 0U);
}

TEST(ThreadPool, FuturesOfDiscardedTasksReportABrokenPromise) {
    // A discarded task never runs, so its packaged_task is destroyed unrun.
    // The caller must find out by exception rather than by waiting forever.
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();

    ThreadPool pool(1, "broken");
    (void)pool.submit([gate] { gate.wait(); });

    std::vector<std::future<int>> queued;
    queued.reserve(10);
    for (int i = 0; i < 10; ++i) {
        queued.push_back(pool.submit([i] { return i; }));
    }

    release.set_value();
    pool.shutdownNow();

    int broken = 0;
    for (std::future<int>& future : queued) {
        ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
            << "a discarded task's future never became ready";
        try {
            (void)future.get();
        } catch (const std::future_error& error) {
            EXPECT_EQ(error.code(), std::make_error_code(std::future_errc::broken_promise));
            ++broken;
        }
    }
    EXPECT_GT(broken, 0) << "nothing was actually discarded, so this proved nothing";
}

TEST(ThreadPool, SubmitAfterShutdownNowIsRejected) {
    ThreadPool pool(2, "closed");
    pool.shutdownNow();

    EXPECT_TRUE(pool.stopping());
    EXPECT_THROW((void)pool.submit([] {}), std::runtime_error);
}

TEST(ThreadPool, RepeatedAndMixedShutdownCallsAreSafe) {
    // The destructor calls shutdown(), so any explicit call is by definition
    // followed by a second one. Mixing the two kinds must also be harmless.
    ThreadPool pool(3, "repeat");
    (void)pool.submit([] {});

    pool.shutdownNow();
    pool.shutdownNow();
    pool.shutdown();
    pool.shutdown();

    EXPECT_TRUE(pool.stopping());
    // Workers are joined, not forgotten: the count is a property of the pool.
    EXPECT_EQ(pool.workerCount(), 3U);
}  // ~ThreadPool calls shutdown() a fifth time

TEST(ThreadPool, ConcurrentShutdownCallsAreSafe) {
    // Two threads in joinAll() used to both iterate workers_ and both join the
    // same std::thread, which is undefined behaviour. Under TSan this is the
    // test that would report it.
    ThreadPool pool(4, "racing");
    for (int i = 0; i < 20; ++i) {
        (void)pool.submit([] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
    }

    std::vector<std::thread> callers;
    callers.reserve(6);
    for (int i = 0; i < 6; ++i) {
        callers.emplace_back([&pool, i] {
            if (i % 2 == 0) {
                pool.shutdown();
            } else {
                pool.shutdownNow();
            }
        });
    }
    for (std::thread& caller : callers) {
        caller.join();
    }

    EXPECT_TRUE(pool.stopping());
    EXPECT_EQ(pool.queueDepth(), 0U);
}

TEST(ThreadPool, WorkersStopConsumingWorkAfterShutdownNow) {
    // "Worker threads terminate cleanly": if any worker were still alive and
    // looping, this second pool's tasks would be competing for a CPU with a
    // spinner, and stopping() would not be observable as final state.
    std::atomic<int> ran{0};
    {
        ThreadPool pool(2, "terminate");
        for (int i = 0; i < 5; ++i) {
            (void)pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
        }
        pool.shutdownNow();
        // shutdownNow() joined, so no worker can touch `ran` after this point.
    }
    const int settled = ran.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(ran.load(std::memory_order_relaxed), settled)
        << "a worker was still running tasks after shutdownNow() returned";
}

TEST(ThreadPool, ConcurrentSubmissionIsSafe) {
    // Submitting from many threads at once is what the runner does; this is
    // the test that would catch a missing lock on the queue.
    ThreadPool pool(4);
    std::atomic<int> executed{0};

    std::vector<std::thread> submitters;
    for (int t = 0; t < 8; ++t) {
        submitters.emplace_back([&pool, &executed] {
            for (int i = 0; i < 50; ++i) {
                (void)pool.submit([&executed] { executed.fetch_add(1); });
            }
        });
    }
    for (std::thread& thread : submitters) {
        thread.join();
    }
    pool.waitIdle();
    EXPECT_EQ(executed.load(), 400);
}

// ---------------------------------------------------------------------------
// CancellationToken
// ---------------------------------------------------------------------------

TEST(Cancellation, StartsUncancelled) {
    const CancellationSource source;
    const CancellationToken token = source.token();
    EXPECT_FALSE(token.isCancelled());
    EXPECT_TRUE(token.reason().empty());
}

TEST(Cancellation, CancelIsVisibleWithItsReason) {
    CancellationSource source;
    const CancellationToken token = source.token();
    source.cancel("deadline exceeded");

    EXPECT_TRUE(token.isCancelled());
    EXPECT_EQ(token.reason(), "deadline exceeded");
}

TEST(Cancellation, FirstReasonWins) {
    CancellationSource source;
    source.cancel("deadline exceeded");
    source.cancel("run aborted");
    // The specific reason must not be overwritten by a later blanket one.
    EXPECT_EQ(source.token().reason(), "deadline exceeded");
}

TEST(Cancellation, WaitForReturnsTrueWhenUninterrupted) {
    const CancellationSource source;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(source.token().waitFor(Milliseconds{30}));
    EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(25));
}

TEST(Cancellation, WaitForWakesImmediatelyOnCancel) {
    CancellationSource source;
    const CancellationToken token = source.token();

    std::thread canceller([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        source.cancel("stop");
    });

    const auto started = std::chrono::steady_clock::now();
    const bool completed = token.waitFor(Milliseconds{5000});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    canceller.join();

    EXPECT_FALSE(completed);
    // The point of cooperative cancellation: a sleeping test stops promptly
    // rather than after its full sleep.
    EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(Cancellation, DeadlineCountsDown) {
    CancellationSource source;
    source.setDeadline(SteadyClock::now() + Milliseconds{200});
    const Milliseconds remaining = source.token().remaining();
    EXPECT_GT(remaining.count(), 0);
    EXPECT_LE(remaining.count(), 200);
}

TEST(Cancellation, DefaultTokenIsInert) {
    // A default-constructed token stands in for "no cancellation configured";
    // it must not crash or claim to be cancelled.
    const CancellationToken token;
    EXPECT_FALSE(token.valid());
    EXPECT_FALSE(token.isCancelled());
    EXPECT_EQ(token.remaining(), Milliseconds::max());
}

TEST(Cancellation, TokenOutlivesItsSource) {
    CancellationToken token;
    {
        CancellationSource source;
        token = source.token();
        source.cancel("gone");
    }
    // The shared state is owned by the token too, so reading it after the
    // source is destroyed is safe.
    EXPECT_TRUE(token.isCancelled());
    EXPECT_EQ(token.reason(), "gone");
}

// ---------------------------------------------------------------------------
// FailureClassifier
// ---------------------------------------------------------------------------

TEST(Classifier, AssertionFailureIsAFailureNotAnError) {
    const AssertionFailure failure("mismatch", "a == b", "1", "2", SourceLocation{});
    const auto classification = FailureClassifier::fromException(failure);

    // The distinction that matters: FAILED means the system under test is
    // wrong; ERROR means the test could not render a verdict.
    EXPECT_EQ(classification.status, TestStatus::Failed);
    EXPECT_EQ(classification.category, FailureCategory::AssertionFailure);
}

TEST(Classifier, SkipIsNotAFailure) {
    const auto classification = FailureClassifier::fromException(SkipTest("no GPU"));
    EXPECT_EQ(classification.status, TestStatus::Skipped);
    EXPECT_EQ(classification.category, FailureCategory::None);
}

TEST(Classifier, TypedErrorsCarryTheirOwnCategory) {
    struct Case {
        std::function<void()> thrower;
        FailureCategory category;
        TestStatus status;
    };

    const std::vector<Case> cases = {
        {[] { throw NetworkError("refused"); }, FailureCategory::NetworkFailure, TestStatus::Error},
        {[] { throw DependencyError("missing"); },
         FailureCategory::DependencyFailure,
         TestStatus::Error},
        {[] { throw EnvironmentError("denied"); },
         FailureCategory::EnvironmentFailure,
         TestStatus::Error},
        {[] { throw ResourceError("oom"); }, FailureCategory::ResourceFailure, TestStatus::Error},
        {[] { throw ConfigurationError("bad"); },
         FailureCategory::ConfigurationFailure,
         TestStatus::Error},
        {[] { throw FrameworkError("bug"); }, FailureCategory::FrameworkError, TestStatus::Error},
        {[] { throw TestCancelled("deadline"); }, FailureCategory::Timeout, TestStatus::Timeout},
    };

    for (const Case& testCase : cases) {
        try {
            testCase.thrower();
            FAIL() << "the case did not throw";
        } catch (const std::exception& error) {
            const auto classification = FailureClassifier::fromException(error);
            EXPECT_EQ(classification.category, testCase.category);
            EXPECT_EQ(classification.status, testCase.status);
        }
    }
}

TEST(Classifier, UnknownExceptionsAreClassifiedFromTheirMessage) {
    const auto refused =
        FailureClassifier::fromException(std::runtime_error("connect: Connection refused"));
    EXPECT_EQ(refused.category, FailureCategory::NetworkFailure);

    const auto space = FailureClassifier::fromException(std::runtime_error("no space left"));
    EXPECT_EQ(space.category, FailureCategory::ResourceFailure);

    const auto unknown = FailureClassifier::fromException(std::runtime_error("something odd"));
    EXPECT_EQ(unknown.category, FailureCategory::Unknown);
}

TEST(Classifier, BadAllocIsAResourceFailure) {
    const auto classification = FailureClassifier::fromException(std::bad_alloc());
    EXPECT_EQ(classification.category, FailureCategory::ResourceFailure);
}

TEST(Classifier, NonStdExceptionIsHandledGracefully) {
    const auto classification = FailureClassifier::fromUnknownException();
    EXPECT_EQ(classification.status, TestStatus::Error);
    EXPECT_EQ(classification.category, FailureCategory::Unknown);
    EXPECT_FALSE(classification.message.empty());
    EXPECT_FALSE(classification.detail.empty());
}

TEST(Classifier, MessageIsOneLineAndDetailKeepsTheRest) {
    const auto classification =
        FailureClassifier::fromException(std::runtime_error("first line\nsecond line"));
    EXPECT_EQ(classification.message, "first line");
    EXPECT_NE(classification.detail.find("second line"), std::string::npos);
}

TEST(Classifier, RefinePromotesAssertionToApplicationFailureOn5xx) {
    json::Value metadata = json::Value::object();
    metadata.set("http_status", 503);

    // The key behaviour: a failed assertion against a 503 is the service's
    // fault, not the test's.
    EXPECT_EQ(FailureClassifier::refine(FailureCategory::AssertionFailure, metadata),
              FailureCategory::ApplicationFailure);
}

TEST(Classifier, RefineLeaves4xxAsAnAssertionFailure) {
    json::Value metadata = json::Value::object();
    metadata.set("http_status", 404);
    EXPECT_EQ(FailureClassifier::refine(FailureCategory::AssertionFailure, metadata),
              FailureCategory::AssertionFailure);
}

TEST(Classifier, RefineNeverMakesADiagnosisVaguer) {
    json::Value metadata = json::Value::object();
    metadata.set("http_status", 500);
    // Timeout is more specific than a generic application failure; refinement
    // must not downgrade it.
    EXPECT_EQ(FailureClassifier::refine(FailureCategory::Timeout, metadata),
              FailureCategory::Timeout);
}

TEST(Classifier, RefineReadsTransportErrors) {
    json::Value metadata = json::Value::object();
    metadata.set("transport_error", "connect failed: Connection refused");
    EXPECT_EQ(FailureClassifier::refine(FailureCategory::Unknown, metadata),
              FailureCategory::NetworkFailure);
}

TEST(Classifier, EveryCategoryHasAnExplanation) {
    for (const FailureCategory category : allFailureCategories()) {
        EXPECT_FALSE(FailureClassifier::explain(category).empty()) << toString(category);
    }
}

TEST(Classifier, OnlyTransientCategoriesAreRetryable) {
    EXPECT_TRUE(FailureClassifier::isRetryable(FailureCategory::NetworkFailure));
    EXPECT_TRUE(FailureClassifier::isRetryable(FailureCategory::Timeout));
    EXPECT_TRUE(FailureClassifier::isRetryable(FailureCategory::ResourceFailure));

    // Retrying a deterministic failure wastes time and hides the signal.
    EXPECT_FALSE(FailureClassifier::isRetryable(FailureCategory::AssertionFailure));
    EXPECT_FALSE(FailureClassifier::isRetryable(FailureCategory::ApplicationFailure));
    EXPECT_FALSE(FailureClassifier::isRetryable(FailureCategory::ConfigurationFailure));
}
