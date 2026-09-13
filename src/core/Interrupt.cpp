#include "testforge/core/Interrupt.hpp"

#include <atomic>
#include <csignal>

namespace testforge::interrupt {
namespace {

/// std::sig_atomic_t is the type the standard guarantees a handler may write,
/// but it says nothing about visibility to other threads. std::atomic<int> is
/// the portable way to get both, and the static_assert below rejects any
/// platform where it would not be signal-safe.
std::atomic<int> gSignal{0};

static_assert(std::atomic<int>::is_always_lock_free,
              "a lock-free atomic is required: a signal handler must never block");

extern "C" void handleInterrupt(int signalNumber) {
    // The only work done here. No allocation, no logging, no locks: a handler
    // runs on whatever thread the kernel chose, possibly inside malloc.
    int expected = 0;
    gSignal.compare_exchange_strong(
        expected, signalNumber, std::memory_order_release, std::memory_order_relaxed);
}

}  // namespace

void installHandlers() {
    std::signal(SIGINT, handleInterrupt);
    std::signal(SIGTERM, handleInterrupt);
#if !defined(_WIN32)
    // Writing to a closed pipe (`testforge list | head`) would otherwise kill
    // the process before it could exit cleanly.
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

bool requested() noexcept {
    return gSignal.load(std::memory_order_acquire) != 0;
}

int signalNumber() noexcept {
    return gSignal.load(std::memory_order_acquire);
}

void reset() noexcept {
    gSignal.store(0, std::memory_order_release);
}

}  // namespace testforge::interrupt
