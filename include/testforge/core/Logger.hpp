#pragma once

#include "testforge/core/Clock.hpp"
#include "testforge/core/Json.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace testforge {

enum class LogLevel : std::uint8_t { Trace = 0, Debug, Info, Warn, Error, Off };

std::string_view toString(LogLevel level) noexcept;

/// Parses "info", "WARN", "error"... Returns nullopt for anything else.
std::optional<LogLevel> logLevelFromString(std::string_view text) noexcept;

/// One structured log event.
///
/// The `fields` member is what makes these logs useful for triage: a log line
/// is not just prose, it carries the run id, test name and any numbers that
/// explain what happened, so it can be grepped, parsed, and correlated with a
/// row in the results database.
struct LogRecord {
    TimePoint timestamp;
    LogLevel level = LogLevel::Info;
    std::string component;
    std::string message;
    json::Object fields;
    std::string runId;
    std::string test;
    std::uint64_t threadId = 0;
};

/// Destination for log records. Implementations must be thread-safe: the
/// LogManager fans out to sinks while holding no lock of its own beyond the
/// sink list.
class LogSink {
 public:
    virtual ~LogSink() = default;

    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;
    LogSink(LogSink&&) = delete;
    LogSink& operator=(LogSink&&) = delete;

    virtual void write(const LogRecord& record) = 0;

    virtual void flush() = 0;

 protected:
    LogSink() = default;
};

/// Writes to stderr. Human-readable by default; `json` switches to one JSON
/// object per line, which is what CI log collectors want.
class ConsoleLogSink final : public LogSink {
 public:
    ConsoleLogSink(bool useJson, bool useColor);

    void write(const LogRecord& record) override;
    void flush() override;

 private:
    bool json_;
    bool color_;
    std::mutex mutex_;
};

/// Appends newline-delimited JSON to a file. Always JSON: files are for
/// machines, the console is for humans.
class FileLogSink final : public LogSink {
 public:
    explicit FileLogSink(const std::string& path);
    ~FileLogSink() override;

    void write(const LogRecord& record) override;
    void flush() override;

    [[nodiscard]] bool ok() const noexcept { return stream_ != nullptr; }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
    std::string path_;
    std::FILE* stream_ = nullptr;
    std::mutex mutex_;
};

/// Collects records in memory. Used by tests, and by the runner to attach the
/// log tail of a failing test to its TestResult.
class MemoryLogSink final : public LogSink {
 public:
    explicit MemoryLogSink(std::size_t capacity = 1000);

    void write(const LogRecord& record) override;
    void flush() override;

    [[nodiscard]] std::vector<LogRecord> records() const;

    [[nodiscard]] std::vector<std::string> renderedLines() const;

    void clear();

 private:
    mutable std::mutex mutex_;
    std::vector<LogRecord> records_;
    std::size_t capacity_;
};

/// Process-wide sink registry and level threshold.
///
/// Deliberately a singleton: logging configuration is genuinely global state,
/// and threading a logger through every constructor would add noise without
/// adding testability (tests swap sinks via replaceSinks instead).
class LogManager {
 public:
    static LogManager& instance();

    void setLevel(LogLevel level) noexcept;

    [[nodiscard]] LogLevel level() const noexcept;

    [[nodiscard]] bool enabled(LogLevel level) const noexcept;

    void addSink(std::shared_ptr<LogSink> sink);

    /// Atomically replaces every sink. Returns the previous set so a caller
    /// can restore it — this is how the unit tests capture output.
    std::vector<std::shared_ptr<LogSink>> replaceSinks(std::vector<std::shared_ptr<LogSink>> sinks);

    void submit(const LogRecord& record);

    void flush();

    /// Correlation id stamped onto every record. Set once per run.
    void setRunId(std::string runId);

    [[nodiscard]] std::string runId() const;

    /// Renders a record the way ConsoleLogSink would in human mode. Exposed so
    /// that MemoryLogSink and the reporters agree on formatting.
    static std::string renderHuman(const LogRecord& record, bool color);

    static json::Value toJson(const LogRecord& record);

 private:
    LogManager() = default;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<LogSink>> sinks_;
    std::atomic<LogLevel> level_{LogLevel::Info};
    std::string runId_;
};

/// Captures every log record emitted **on the current thread** for as long as
/// the object lives.
///
/// This is how the runner attaches a log tail to a failing TestResult. Because
/// a test executes on exactly one thread, thread-local capture gives clean
/// per-test attribution with no locking on the hot path and no interleaving
/// between concurrently running tests. Captures nest.
class ScopedLogCapture {
 public:
    explicit ScopedLogCapture(std::size_t capacity = 500);
    ~ScopedLogCapture();

    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;
    ScopedLogCapture(ScopedLogCapture&&) = delete;
    ScopedLogCapture& operator=(ScopedLogCapture&&) = delete;

    [[nodiscard]] std::vector<LogRecord> records() const { return records_; }

    /// Rendered, human-readable lines — what ends up in TestResult::logs.
    [[nodiscard]] std::vector<std::string> lines() const;

 private:
    friend class LogManager;

    std::vector<LogRecord> records_;
    std::size_t capacity_;
};

/// Lightweight handle bound to a component name.
///
/// Copying a Logger is cheap and safe; it holds no resources, only the
/// component label and an optional test correlation.
class Logger {
 public:
    explicit Logger(std::string component) : component_(std::move(component)) {}

    /// Returns a copy bound to the given test name, so every record emitted
    /// through it is attributable to that test.
    [[nodiscard]] Logger forTest(std::string testName) const;

    [[nodiscard]] Logger child(std::string_view suffix) const;

    void log(LogLevel level,
             std::string_view message,
             std::initializer_list<json::Member> fields = {}) const;

    void trace(std::string_view message, std::initializer_list<json::Member> fields = {}) const;
    void debug(std::string_view message, std::initializer_list<json::Member> fields = {}) const;
    void info(std::string_view message, std::initializer_list<json::Member> fields = {}) const;
    void warn(std::string_view message, std::initializer_list<json::Member> fields = {}) const;
    void error(std::string_view message, std::initializer_list<json::Member> fields = {}) const;

    [[nodiscard]] const std::string& component() const noexcept { return component_; }

 private:
    std::string component_;
    std::string test_;
};

}  // namespace testforge
