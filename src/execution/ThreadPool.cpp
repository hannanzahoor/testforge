#include "testforge/execution/ThreadPool.hpp"

#include "testforge/core/Logger.hpp"

#include <algorithm>

namespace testforge {
namespace {

/// Name of the pool worker on this thread, or "main" elsewhere. Thread-local
/// so that TestResult::worker can be filled in without plumbing the worker
/// index through every layer.
std::string& workerNameSlot() {
    static thread_local std::string name = "main";
    return name;
}

}  // namespace

const std::string& ThreadPool::currentWorkerName() {
    return workerNameSlot();
}

ThreadPool::ThreadPool(std::size_t workers, std::string name) : name_(std::move(name)) {
    const std::size_t count = std::max<std::size_t>(1, workers);
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this, i] { workerLoop(i); });
    }
}

ThreadPool::~ThreadPool() {
    // Default to draining: a destructor that silently threw away queued tests
    // would lose results, which is worse than waiting.
    shutdown();
}

void ThreadPool::workerLoop(std::size_t index) {
    workerNameSlot() = name_ + "-" + std::to_string(index);

    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workAvailable_.wait(lock, [this] {
                return !tasks_.empty() || stopping_.load(std::memory_order_acquire);
            });

            if (tasks_.empty()) {
                // Woken by shutdown with nothing left to do.
                return;
            }
            if (stopping_.load(std::memory_order_acquire) && discardQueued_) {
                // shutdownNow(): drop the remaining queue and account for it so
                // waitIdle() cannot hang.
                while (!tasks_.empty()) {
                    tasks_.pop();
                    --pending_;
                }
                idle_.notify_all();
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // A packaged_task captures exceptions into its future, so this should
        // not throw. The catch-all is a backstop: letting an exception escape
        // here would call std::terminate and take the whole run down.
        try {
            task();
        } catch (const std::exception& error) {
            Logger("threadpool")
                .error("task threw outside packaged_task", {{"error", std::string(error.what())}});
        } catch (...) {
            Logger("threadpool").error("task threw a non-std exception");
        }

        {
            const std::lock_guard<std::mutex> lock(mutex_);
            --pending_;
            if (pending_ == 0) {
                idle_.notify_all();
            }
        }
    }
}

void ThreadPool::waitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return pending_ == 0; });
}

std::size_t ThreadPool::queueDepth() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void ThreadPool::shutdown() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true, std::memory_order_release);
        discardQueued_ = false;  // drain, as opposed to shutdownNow()
    }
    workAvailable_.notify_all();
    joinAll();
}

void ThreadPool::shutdownNow() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true, std::memory_order_release);
        discardQueued_ = true;
    }
    workAvailable_.notify_all();
    joinAll();
}

void ThreadPool::joinAll() {
    // Serialised, and idempotent.
    //
    // Two threads calling shutdown() at once would otherwise both iterate
    // workers_ and both join the same std::thread, which is undefined. The
    // second caller now blocks here and returns once the first has finished,
    // which is what a caller of a function named "shutdown" expects.
    //
    // joinMutex_ rather than mutex_: the workers being joined need mutex_ to
    // finish their current task, so joining while holding it would deadlock.
    const std::lock_guard<std::mutex> lock(joinMutex_);
    if (joined_) {
        return;
    }
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    // workers_ is deliberately not cleared: leaving it intact keeps
    // workerCount() a race-free read of a vector nothing mutates after
    // construction.
    joined_ = true;
}

}  // namespace testforge
