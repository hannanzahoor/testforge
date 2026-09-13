#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace testforge {

/// Fixed-size worker pool.
///
/// Concurrency model (docs/concurrency.md has the full write-up):
///
///   * N worker threads are created in the constructor and joined in the
///     destructor. Threads are never created per task, so a 10 000-test run
///     costs N threads, not 10 000.
///   * One mutex guards the queue and the stopping flag; a condition_variable
///     parks idle workers so they consume no CPU.
///   * submit() returns a std::future, so a caller can wait for one specific
///     task without polling.
///   * The destructor drains the queue by default (join semantics). Call
///     shutdownNow() to discard queued work instead.
///
/// Exception safety: a task that throws does not kill its worker. The
/// exception is captured into the task's future, exactly as std::async would.
class ThreadPool {
 public:
    /// `workers` is clamped to at least 1. `name` prefixes worker labels,
    /// which appear in logs and in TestResult::worker.
    explicit ThreadPool(std::size_t workers, std::string name = "worker");

    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Enqueues `callable` and returns a future for its result.
    /// Throws std::runtime_error if the pool is already stopping.
    template<typename Callable, typename... Args>
    auto submit(Callable&& callable, Args&&... args)
        -> std::future<std::invoke_result_t<Callable, Args...>>;

    /// Blocks until every queued and running task has finished. The pool
    /// remains usable afterwards.
    void waitIdle();

    /// Stops accepting work, lets running and queued tasks finish, joins.
    ///
    /// Safe to call more than once, and safe to call from two threads at the
    /// same time: the second caller blocks until the first has finished
    /// joining, then returns. The destructor calls it, so an explicit call
    /// followed by destruction is the normal case rather than an error.
    void shutdown();

    /// Stops accepting work, discards anything still queued, waits only for
    /// tasks already running, joins.
    ///
    /// A discarded task never runs. Its packaged_task is destroyed unrun, so
    /// a caller waiting on the future it returned gets a
    /// std::future_error(broken_promise) rather than a value or a hang.
    void shutdownNow();

    /// Number of worker threads. Fixed for the lifetime of the pool, including
    /// after shutdown: the threads are joined, not forgotten.
    [[nodiscard]] std::size_t workerCount() const noexcept { return workers_.size(); }

    [[nodiscard]] std::size_t queueDepth() const;

    [[nodiscard]] bool stopping() const noexcept {
        return stopping_.load(std::memory_order_acquire);
    }

    /// Label of the worker running the calling thread, e.g. "worker-3".
    /// Returns "main" when called from a non-pool thread.
    static const std::string& currentWorkerName();

 private:
    void workerLoop(std::size_t index);

    void joinAll();

    /// Written only by the constructor, so every later read is race-free.
    std::vector<std::thread> workers_;
    std::string name_;

    mutable std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable idle_;
    std::queue<std::function<void()>> tasks_;

    /// Separate from mutex_ on purpose: joinAll() waits for worker threads to
    /// exit, and those threads need mutex_ to finish their last task. Joining
    /// under mutex_ would deadlock.
    std::mutex joinMutex_;
    bool joined_ = false;  ///< guarded by joinMutex_

    std::atomic<bool> stopping_{false};
    bool discardQueued_ = false;
    std::size_t pending_ = 0;  ///< queued + running, guarded by mutex_
};

template<typename Callable, typename... Args>
auto ThreadPool::submit(Callable&& callable, Args&&... args)
    -> std::future<std::invoke_result_t<Callable, Args...>> {
    using Result = std::invoke_result_t<Callable, Args...>;

    // packaged_task lives in a shared_ptr because std::function requires a
    // copyable target and packaged_task is move-only.
    auto task = std::make_shared<std::packaged_task<Result()>>(
        [callable = std::forward<Callable>(callable),
         argsTuple = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Result {
            return std::apply(std::move(callable), std::move(argsTuple));
        });

    std::future<Result> future = task->get_future();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load(std::memory_order_acquire)) {
            throw std::runtime_error("ThreadPool::submit called after shutdown");
        }
        tasks_.emplace([task]() { (*task)(); });
        ++pending_;
    }
    workAvailable_.notify_one();
    return future;
}

}  // namespace testforge
