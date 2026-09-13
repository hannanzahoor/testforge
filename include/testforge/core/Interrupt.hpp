#pragma once

/// \file
/// Cooperative interrupt handling for SIGINT and SIGTERM.
///
/// A test runner and a long-lived server both need Ctrl-C to mean "stop
/// cleanly", not "die mid-write". Killing the process outright can leave a
/// half-written report on disk, a SQLite transaction open, and a run row that
/// claims to be in progress forever.
///
/// So the handler does the only thing a signal handler may safely do: it sets
/// a flag. Everything that could block for a noticeable time polls
/// requested() and unwinds normally.
///
/// This exists as a module rather than a static in main.cpp because the flag
/// is useless to the code that has to act on it if only main can see it. An
/// earlier version of this codebase installed the handlers in main.cpp and
/// never read the flag anywhere, which was worse than installing nothing:
/// replacing SIGINT's default disposition without honouring it made Ctrl-C
/// stop working altogether.

namespace testforge::interrupt {

/// Installs handlers for SIGINT and SIGTERM. Idempotent.
///
/// Also ignores SIGPIPE on POSIX, so writing to a closed pipe
/// (`testforge list | head`) returns EPIPE instead of killing the process.
void installHandlers();

/// True once SIGINT or SIGTERM has been received.
///
/// Acquire ordering, so everything the signal handler did before setting the
/// flag is visible to the thread that observes it.
[[nodiscard]] bool requested() noexcept;

/// The signal that caused the interrupt, or 0 if none has arrived.
[[nodiscard]] int signalNumber() noexcept;

/// Clears the flag. Exists for tests; production code has no reason to
/// un-request a shutdown.
void reset() noexcept;

}  // namespace testforge::interrupt
