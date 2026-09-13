#pragma once

#include "testforge/core/Clock.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace testforge {

/// Cooperative cancellation.
///
/// TestForge never force-kills a thread. pthread_cancel and friends leave
/// mutexes locked, destructors unrun and the heap in an unknown state — in a
/// test *tool*, whose job is to report trustworthy results, that trade is not
/// worth making. Instead a timed-out or aborted test is asked to stop, and it
/// stops at its next checkpoint.
///
/// Tests cooperate in three ways:
///   * calling TestContext::throwIfCancelled() between steps,
///   * sleeping via CancellationToken::waitFor() rather than
///     std::this_thread::sleep_for(),
///   * passing the token to the HTTP client, which aborts long reads.
///
/// docs/concurrency.md documents what happens when a test does not cooperate.
class CancellationSource;

class CancellationToken {
 public:
    CancellationToken() = default;

    [[nodiscard]] bool isCancelled() const noexcept;

    [[nodiscard]] std::string reason() const;

    /// Sleeps for `duration` or until cancelled, whichever comes first.
    /// Returns true if the full duration elapsed, false if cancelled.
    [[nodiscard]] bool waitFor(Milliseconds duration) const;

    /// Deadline remaining before an external timeout, if one was set.
    /// Milliseconds::max() when unbounded.
    [[nodiscard]] Milliseconds remaining() const;

    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

 private:
    friend class CancellationSource;

    struct State {
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        std::condition_variable condition;
        std::string reason;
        SteadyClock::time_point deadline{SteadyClock::time_point::max()};
    };

    explicit CancellationToken(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::shared_ptr<State> state_;
};

/// The writable side of a cancellation channel. Held by the runner; tests only
/// ever see the read-only CancellationToken.
class CancellationSource {
 public:
    CancellationSource() : state_(std::make_shared<CancellationToken::State>()) {}

    CancellationSource(const CancellationSource&) = delete;
    CancellationSource& operator=(const CancellationSource&) = delete;
    CancellationSource(CancellationSource&&) noexcept = default;
    CancellationSource& operator=(CancellationSource&&) noexcept = default;
    ~CancellationSource() = default;

    [[nodiscard]] CancellationToken token() const { return CancellationToken(state_); }

    /// Idempotent: the first reason wins, so a later blanket "run aborted"
    /// cannot overwrite the more specific "deadline exceeded".
    void cancel(std::string reason);

    void setDeadline(SteadyClock::time_point deadline);

    [[nodiscard]] bool isCancelled() const noexcept {
        return state_->cancelled.load(std::memory_order_acquire);
    }

 private:
    std::shared_ptr<CancellationToken::State> state_;
};

}  // namespace testforge
