#include "testforge/core/Process.hpp"

#include "testforge/core/Environment.hpp"
#include "testforge/core/StringUtils.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <vector>

#if defined(_WIN32)

namespace testforge {

ProcessResult ProcessRunner::run(const std::string& executable,
                                 const std::vector<std::string>& /*arguments*/,
                                 const ProcessOptions& /*options*/) {
    ProcessResult result;
    result.started = false;
    result.launchError =
        "external process execution is implemented for POSIX only; '" + executable + "' not run";
    return result;
}

std::string ProcessRunner::which(const std::string& /*executable*/) {
    return {};
}

bool ProcessRunner::isAvailable(const std::string& /*executable*/) {
    return false;
}

bool ProcessRunner::isSafeArgument(const std::string& argument) noexcept {
    return argument.find('\0') == std::string::npos;
}

}  // namespace testforge

#else  // POSIX

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace testforge {
namespace {

/// RAII wrapper for a pipe. Guarantees both descriptors are closed on every
/// path, including the ones taken when fork() or execvp() fails.
class Pipe {
 public:
    Pipe() {
        if (::pipe(fds_) != 0) {
            fds_[0] = -1;
            fds_[1] = -1;
        }
    }

    ~Pipe() {
        closeRead();
        closeWrite();
    }

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&&) = delete;
    Pipe& operator=(Pipe&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return fds_[0] >= 0 && fds_[1] >= 0; }

    [[nodiscard]] int readFd() const noexcept { return fds_[0]; }

    [[nodiscard]] int writeFd() const noexcept { return fds_[1]; }

    void closeRead() noexcept {
        if (fds_[0] >= 0) {
            ::close(fds_[0]);
            fds_[0] = -1;
        }
    }

    void closeWrite() noexcept {
        if (fds_[1] >= 0) {
            ::close(fds_[1]);
            fds_[1] = -1;
        }
    }

 private:
    int fds_[2] = {-1, -1};
};

/// EAGAIN and EWOULDBLOCK have the same value on Linux, but POSIX does not
/// require that, so both must be checked for portability. Writing the
/// comparison inline makes GCC warn about a tautology; keeping it here
/// documents the intent and compiles to the right thing on either kind of
/// platform.
constexpr bool wouldBlock(int error) noexcept {
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
    return error == EAGAIN || error == EWOULDBLOCK;
#else
    return error == EAGAIN;
#endif
}

bool isExecutableFile(const std::string& path) {
    struct ::stat info {};

    if (::stat(path.c_str(), &info) != 0) {
        return false;
    }
    if (!S_ISREG(info.st_mode)) {
        return false;
    }
    return ::access(path.c_str(), X_OK) == 0;
}

/// Drains `fd` into `sink` until it would block. Returns false when EOF was
/// reached or the descriptor errored, so the caller can stop polling it.
bool drain(int fd, std::string& sink, std::size_t maxBytes, bool& capped) {
    std::array<char, 8192> buffer{};
    while (true) {
        const ssize_t got = ::read(fd, buffer.data(), buffer.size());
        if (got > 0) {
            const auto count = static_cast<std::size_t>(got);
            if (sink.size() + count > maxBytes) {
                const std::size_t room = maxBytes > sink.size() ? maxBytes - sink.size() : 0;
                sink.append(buffer.data(), room);
                capped = true;
                return false;
            }
            sink.append(buffer.data(), count);
            continue;
        }
        if (got == 0) {
            return false;  // EOF
        }
        if (errno == EINTR) {
            continue;
        }
        if (wouldBlock(errno)) {
            return true;  // nothing more for now
        }
        return false;  // real error
    }
}

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

}  // namespace

bool ProcessRunner::isSafeArgument(const std::string& argument) noexcept {
    return argument.find('\0') == std::string::npos && argument.find('\n') == std::string::npos;
}

std::string ProcessRunner::which(const std::string& executable) {
    if (executable.empty()) {
        return {};
    }
    // A name containing a slash is a path: use it directly, never search PATH.
    if (executable.find('/') != std::string::npos) {
        return isExecutableFile(executable) ? executable : std::string{};
    }
    const std::string pathVar = env::getOr("PATH", "/usr/local/bin:/usr/bin:/bin");
    for (const std::string& dir : strings::split(pathVar, ':', true)) {
        std::string candidate = dir;
        if (!candidate.empty() && candidate.back() != '/') {
            candidate.push_back('/');
        }
        candidate += executable;
        if (isExecutableFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

bool ProcessRunner::isAvailable(const std::string& executable) {
    return !which(executable).empty();
}

ProcessResult ProcessRunner::run(const std::string& executable,
                                 const std::vector<std::string>& arguments,
                                 const ProcessOptions& options) {
    ProcessResult result;
    Stopwatch watch;

    if (!isSafeArgument(executable)) {
        result.launchError = "executable name contains an illegal character";
        return result;
    }
    for (const std::string& argument : arguments) {
        if (!isSafeArgument(argument)) {
            result.launchError = "argument contains an illegal character";
            return result;
        }
    }

    const std::string resolved = which(executable);
    if (resolved.empty()) {
        result.launchError = "executable not found on PATH: " + executable;
        return result;
    }

    Pipe outPipe;
    Pipe errPipe;
    if (!outPipe.valid() || !errPipe.valid()) {
        result.launchError = std::string("pipe() failed: ") + std::strerror(errno);
        return result;
    }

    // Build argv before forking. Everything the child touches between fork and
    // exec must be async-signal-safe, which rules out allocating there.
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    std::string program = resolved;
    argv.push_back(program.data());
    std::vector<std::string> argStorage = arguments;
    for (std::string& argument : argStorage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> envStorage;
    std::vector<char*> envp;
    const bool customEnv = !options.extraEnvironment.empty();
    if (customEnv) {
        for (char** entry = ::environ; entry != nullptr && *entry != nullptr; ++entry) {
            envStorage.emplace_back(*entry);
        }
        for (const auto& [key, value] : options.extraEnvironment) {
            envStorage.push_back(key + "=" + value);
        }
        envp.reserve(envStorage.size() + 1);
        for (std::string& entry : envStorage) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);
    }

    const std::string workingDirectory = options.workingDirectory;

    const pid_t pid = ::fork();
    if (pid < 0) {
        result.launchError = std::string("fork() failed: ") + std::strerror(errno);
        return result;
    }

    if (pid == 0) {
        // ---------------- child ----------------
        // Only async-signal-safe calls from here until execvp().
        ::setpgid(0, 0);  // own process group, so a timeout can kill descendants

        if (::dup2(outPipe.writeFd(), STDOUT_FILENO) < 0) {
            ::_exit(127);
        }
        const int errTarget = options.captureStandardError ? errPipe.writeFd() : outPipe.writeFd();
        if (::dup2(errTarget, STDERR_FILENO) < 0) {
            ::_exit(127);
        }
        ::close(outPipe.readFd());
        ::close(errPipe.readFd());
        ::close(outPipe.writeFd());
        ::close(errPipe.writeFd());

        // Detach stdin: a diagnostic command must never block waiting for
        // input, and must never consume the parent's stdin.
        const int devNull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            ::close(devNull);
        }

        if (!workingDirectory.empty() && ::chdir(workingDirectory.c_str()) != 0) {
            ::_exit(126);
        }

        // execvp, not system/popen: the argv array is passed straight to the
        // kernel with no shell in between.
        if (customEnv) {
            ::execve(program.c_str(), argv.data(), envp.data());
        } else {
            ::execv(program.c_str(), argv.data());
        }
        ::_exit(127);  // exec failed
    }

    // ---------------- parent ----------------
    outPipe.closeWrite();
    errPipe.closeWrite();
    setNonBlocking(outPipe.readFd());
    setNonBlocking(errPipe.readFd());

    result.started = true;

    bool outOpen = true;
    bool errOpen = options.captureStandardError;
    if (!errOpen) {
        errPipe.closeRead();
    }
    bool capped = false;

    const auto deadline = SteadyClock::now() + options.timeout;
    bool killSent = false;
    bool hardKillSent = false;
    SteadyClock::time_point killAt{};

    int waitStatus = 0;
    bool reaped = false;

    while (true) {
        std::array<::pollfd, 2> fds{};
        int count = 0;
        if (outOpen) {
            fds[static_cast<std::size_t>(count)] = {outPipe.readFd(), POLLIN, 0};
            ++count;
        }
        if (errOpen) {
            fds[static_cast<std::size_t>(count)] = {errPipe.readFd(), POLLIN, 0};
            ++count;
        }

        // Poll in short slices so that the deadline is honoured even when the
        // child produces no output at all.
        const int pollTimeoutMs = 50;
        if (count > 0) {
            const int ready = ::poll(fds.data(), static_cast<nfds_t>(count), pollTimeoutMs);
            if (ready > 0) {
                int index = 0;
                if (outOpen) {
                    if ((fds[static_cast<std::size_t>(index)].revents &
                         (POLLIN | POLLHUP | POLLERR)) != 0) {
                        if (!drain(outPipe.readFd(),
                                   result.standardOutput,
                                   options.maxOutputBytes,
                                   capped)) {
                            outOpen = false;
                            outPipe.closeRead();
                        }
                    }
                    ++index;
                }
                if (errOpen) {
                    if ((fds[static_cast<std::size_t>(index)].revents &
                         (POLLIN | POLLHUP | POLLERR)) != 0) {
                        if (!drain(errPipe.readFd(),
                                   result.standardError,
                                   options.maxOutputBytes,
                                   capped)) {
                            errOpen = false;
                            errPipe.closeRead();
                        }
                    }
                }
            }
        } else {
            // No descriptors left to watch; sleep briefly rather than spin.
            struct ::timespec nap {
                0, 20L * 1000L * 1000L
            };

            ::nanosleep(&nap, nullptr);
        }

        const pid_t waited = ::waitpid(pid, &waitStatus, WNOHANG);
        if (waited == pid) {
            reaped = true;
            // Drain whatever is still buffered in the pipes after exit.
            if (outOpen) {
                drain(outPipe.readFd(), result.standardOutput, options.maxOutputBytes, capped);
            }
            if (errOpen) {
                drain(errPipe.readFd(), result.standardError, options.maxOutputBytes, capped);
            }
            break;
        }
        if (waited < 0 && errno != EINTR) {
            break;
        }

        const auto now = SteadyClock::now();
        const bool expired = options.timeout.count() > 0 && now >= deadline;
        if ((expired || capped) && !killSent) {
            result.timedOut = expired;
            // Signal the whole process group: nvidia-smi and friends may have
            // spawned children of their own.
            ::kill(-pid, SIGTERM);
            killSent = true;
            killAt = now + options.killGrace;
        } else if (killSent && !hardKillSent && now >= killAt) {
            ::kill(-pid, SIGKILL);
            hardKillSent = true;
        }
    }

    if (!reaped) {
        // Make sure we never leave a zombie behind.
        ::kill(-pid, SIGKILL);
        while (::waitpid(pid, &waitStatus, 0) < 0 && errno == EINTR) {
        }
    }

    if (WIFEXITED(waitStatus)) {
        result.exitCode = WEXITSTATUS(waitStatus);
    } else if (WIFSIGNALED(waitStatus)) {
        result.signalled = true;
        result.signalNumber = WTERMSIG(waitStatus);
        result.exitCode = 128 + result.signalNumber;
    }

    if (capped && !result.timedOut) {
        result.standardError += "\n[testforge] output truncated at " +
                                std::to_string(options.maxOutputBytes) + " bytes";
    }

    result.duration = watch.elapsed();
    return result;
}

}  // namespace testforge

#endif  // POSIX
