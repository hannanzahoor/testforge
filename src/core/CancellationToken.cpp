#include "testforge/core/CancellationToken.hpp"

// <thread> for std::this_thread::sleep_for in the null-state branch of
// waitFor(). libstdc++ pulls it in via <condition_variable>, but relying
// on that is how a file stops compiling on a different standard library.
#include <thread>
#include <utility>

namespace testforge {

bool CancellationToken::isCancelled() const noexcept {
    if (!state_) {
        return false;
    }
    return state_->cancelled.load(std::memory_order_acquire);
}

std::string CancellationToken::reason() const {
    if (!state_) {
        return {};
    }
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->reason;
}

bool CancellationToken::waitFor(Milliseconds duration) const {
    if (!state_) {
        std::this_thread::sleep_for(duration);
        return true;
    }
    std::unique_lock<std::mutex> lock(state_->mutex);
    // wait_for with a predicate handles spurious wake-ups; the return value
    // tells us whether the predicate became true (cancelled) or we timed out
    // (slept the whole duration, which is the success case here).
    const bool cancelled = state_->condition.wait_for(
        lock, duration, [this] { return state_->cancelled.load(std::memory_order_acquire); });
    return !cancelled;
}

Milliseconds CancellationToken::remaining() const {
    if (!state_) {
        return Milliseconds::max();
    }
    SteadyClock::time_point deadline;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        deadline = state_->deadline;
    }
    if (deadline == SteadyClock::time_point::max()) {
        return Milliseconds::max();
    }
    const auto now = SteadyClock::now();
    if (now >= deadline) {
        return Milliseconds{0};
    }
    return std::chrono::duration_cast<Milliseconds>(deadline - now);
}

void CancellationSource::cancel(std::string reason) {
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->cancelled.load(std::memory_order_relaxed)) {
            return;  // first reason wins
        }
        state_->reason = std::move(reason);
        // Release ordering pairs with the acquire in isCancelled(), so a
        // thread that observes cancellation also observes the reason.
        state_->cancelled.store(true, std::memory_order_release);
    }
    state_->condition.notify_all();
}

void CancellationSource::setDeadline(SteadyClock::time_point deadline) {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->deadline = deadline;
}

}  // namespace testforge
