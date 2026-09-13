#pragma once

#include "testforge/core/Clock.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace testforge {

/// Outcome of running an external program.
struct ProcessResult {
    int exitCode = -1;
    bool timedOut = false;
    bool signalled = false;
    int signalNumber = 0;
    bool started = false;  ///< false when the executable could not be launched
    std::string standardOutput;
    std::string standardError;
    Milliseconds duration{0};
    std::string launchError;  ///< populated when started == false

    [[nodiscard]] bool ok() const noexcept {
        return started && !timedOut && !signalled && exitCode == 0;
    }
};

struct ProcessOptions {
    /// Hard deadline. On expiry the child is sent SIGTERM, then SIGKILL after
    /// `killGrace`. Zero means "wait forever", which TestForge never uses.
    Milliseconds timeout{5'000};
    Milliseconds killGrace{500};

    /// Output is capped so that a runaway child cannot exhaust memory. Reading
    /// stops once the cap is hit, and the child is terminated.
    std::size_t maxOutputBytes = 1u * 1024u * 1024u;

    bool captureStandardError = true;

    /// Extra variables for the child only. The parent environment is
    /// inherited; nothing here mutates the parent.
    std::vector<std::pair<std::string, std::string>> extraEnvironment;

    std::string workingDirectory;
};

/// Runs external programs safely.
///
/// SECURITY MODEL — this is the only place in TestForge that starts a process,
/// and it is deliberately incapable of invoking a shell:
///
///   * execvp() is called with an argv array. There is no shell, so shell
///     metacharacters in an argument (';', '|', '$(...)', backticks) are inert
///     data — a filename, never a command.
///   * system() and popen() are never used anywhere in this codebase.
///   * The executable is resolved through an explicit PATH search; a name
///     containing '/' is treated as a path and is not searched for.
///   * Arguments are rejected up-front if they contain a NUL byte, which is
///     the one thing that could truncate an argv entry.
///   * Every invocation has a timeout and an output cap.
///
/// See docs/security.md for the full rationale.
class ProcessRunner {
 public:
    /// Runs `executable` with `arguments`. `executable` is either a bare name
    /// resolved against PATH, or a path containing '/'.
    static ProcessResult run(const std::string& executable,
                             const std::vector<std::string>& arguments,
                             const ProcessOptions& options = {});

    /// PATH lookup without touching a shell. Returns an empty string when the
    /// program is not found or is not executable.
    static std::string which(const std::string& executable);

    static bool isAvailable(const std::string& executable);

    /// True when the string contains no NUL and no newline. Callers use this
    /// to validate anything that will become an argv entry; it is a sanity
    /// check for logging clarity rather than a security boundary, because the
    /// absence of a shell is what actually provides the safety.
    static bool isSafeArgument(const std::string& argument) noexcept;
};

}  // namespace testforge
