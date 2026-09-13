#pragma once

#include "testforge/core/Json.hpp"
#include "testforge/core/Logger.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace testforge {

struct ExecutionConfig {
    /// 0 means "decide at run time from hardware_concurrency".
    int workers = 0;
    std::int64_t defaultTimeoutMs = 30'000;

    /// Grace period the runner waits for a timed-out test to notice
    /// cancellation before it gives up on the thread. See docs/concurrency.md.
    std::int64_t cancellationGraceMs = 2'000;

    bool failFast = false;
    int retryFailed = 0;  ///< re-run failures this many times (flake triage)
    bool shuffle = false;
    std::uint64_t shuffleSeed = 0;  ///< 0 = derive from the clock

    /// Suites skipped by a bare `testforge run`.
    ///
    /// This exists for failure_injection: those tests are supposed to fail, so
    /// including them in the default run would make a healthy checkout look
    /// broken. Naming a suite or a tag explicitly overrides the exclusion, so
    /// `--suite failure_injection` still runs it.
    std::vector<std::string> excludeSuitesByDefault = {"failure_injection"};
};

struct LoggingConfig {
    LogLevel level = LogLevel::Info;
    bool json = false;  ///< machine-readable console output
    bool color = true;  ///< disabled automatically when stderr is not a TTY
    std::string file;   ///< empty disables the file sink
};

struct DatabaseConfig {
    bool enabled = true;
    std::string path = "testforge.db";
    /// Runs older than this are pruned by `testforge history --prune`.
    int retentionDays = 90;
    std::int64_t busyTimeoutMs = 5'000;
};

struct ReportingConfig {
    std::string outputDirectory = "reports";
    bool console = true;
    bool json = true;
    bool html = true;
    bool junit = false;
    /// Show the full log tail for each failure in the console report.
    bool verboseFailures = true;
};

struct ApiTargetConfig {
    /// Base URL of the system under test, e.g. "http://127.0.0.1:8000".
    std::string baseUrl = "http://127.0.0.1:8000";
    std::int64_t timeoutMs = 10'000;
    std::int64_t slaMs = 2'000;  ///< default ASSERT_RESPONSE_TIME budget
    std::vector<json::Member> defaultHeaders;
};

struct AiConfig {
    /// AI is opt-in. With this false, every AI code path returns a clearly
    /// labelled "disabled" result rather than silently doing nothing.
    bool enabled = false;

    /// The Python AI sidecar. TestForge never talks to api.openai.com
    /// directly — see docs/ai-architecture.md.
    std::string endpoint = "http://127.0.0.1:8810/";
    std::string model = "gpt-4o-mini";
    std::int64_t timeoutMs = 60'000;

    /// Caps applied to anything the model produces, before validation.
    int maxGeneratedTests = 50;
    std::int64_t maxResponseBytes = 512 * 1024;

    /// Hosts a generated test is allowed to target. Empty means "only the
    /// configured api.baseUrl host".
    std::vector<std::string> allowedTargetHosts;
};

struct DiagnosticsConfig {
    bool collectOnFailure = true;
    bool includeGpu = true;
    bool includeProcesses = false;   ///< off by default: verbose and privacy-sensitive
    bool includeSystemLogs = false;  ///< off by default: usually needs privileges
    int topProcessCount = 10;
    std::int64_t commandTimeoutMs = 4'000;
};

struct ServerConfig {
    std::string host = "127.0.0.1";  ///< loopback by default; see docs/security.md
    int port = 8080;
    int workers = 4;
    std::string staticDirectory = "dashboard";
    std::int64_t maxRequestBytes = 1024 * 1024;

    /// Ceiling on connections accepted but not yet finished.
    ///
    /// The acceptor hands each connection to the worker pool, whose queue is
    /// unbounded. Without a cap, a client that opens sockets faster than the
    /// workers drain them makes the process hold one file descriptor per
    /// queued connection until it runs out — each for up to the socket read
    /// timeout. Past this many, a connection is answered with 503 and closed
    /// immediately, which is a bounded, visible failure rather than descriptor
    /// exhaustion.
    ///
    /// Generous relative to `workers`: the point is to have a limit at all,
    /// not to throttle ordinary traffic.
    int maxPendingConnections = 256;
};

/// Complete TestForge configuration.
///
/// Precedence, lowest to highest: built-in defaults, config file, environment
/// variables, command line flags. Anything secret (API keys) is read from the
/// environment only and never appears in this struct.
class Config {
 public:
    ExecutionConfig execution;
    LoggingConfig logging;
    DatabaseConfig database;
    ReportingConfig reporting;
    ApiTargetConfig api;
    AiConfig ai;
    DiagnosticsConfig diagnostics;
    ServerConfig server;

    /// Free-form values available to tests via TestContext::configValue.
    json::Value custom = json::Value::object();

    [[nodiscard]] static Config defaults() { return {}; }

    /// Reads and parses a JSON config file.
    /// Throws ConfigurationError when the file is unreadable or malformed.
    static Config loadFromFile(const std::string& path);

    static Config fromJson(const json::Value& root);

    /// Applies TESTFORGE_* environment overrides in place.
    void applyEnvironmentOverrides();

    /// Throws ConfigurationError describing every problem found.
    void validate() const;

    [[nodiscard]] json::Value toJson() const;

    /// Effective worker count: resolves `workers == 0` against the hardware.
    [[nodiscard]] int effectiveWorkers() const;

    /// The path a config file was loaded from, or empty for defaults.
    [[nodiscard]] const std::string& sourcePath() const noexcept { return sourcePath_; }

 private:
    std::string sourcePath_;
};

}  // namespace testforge
