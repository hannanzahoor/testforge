#include "testforge/core/Logger.hpp"

#include "testforge/core/Status.hpp"
#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <thread>
#include <utility>

namespace testforge {
namespace {

std::uint64_t currentThreadId() {
    // hash of thread::id gives a stable, printable number without relying on
    // a platform-specific gettid().
    return std::uint64_t{std::hash<std::thread::id>{}(std::this_thread::get_id())};
}

/// Stack of active per-thread captures. A raw pointer stack is safe here
/// because ScopedLogCapture is strictly scope-bound and non-movable, so an
/// entry can never outlive the object it points at.
std::vector<ScopedLogCapture*>& captureStack() {
    static thread_local std::vector<ScopedLogCapture*> stack;
    return stack;
}

std::string_view levelColor(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "\033[90m";
        case LogLevel::Debug:
            return "\033[36m";
        case LogLevel::Info:
            return "\033[32m";
        case LogLevel::Warn:
            return "\033[33m";
        case LogLevel::Error:
            return "\033[31m";
        case LogLevel::Off:
            break;
    }
    return "";
}

}  // namespace

std::string_view toString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Off:
            return "OFF";
    }
    return "INFO";
}

std::optional<LogLevel> logLevelFromString(std::string_view text) noexcept {
    const std::string upper = strings::toUpper(strings::trim(text));
    if (upper == "TRACE")
        return LogLevel::Trace;
    if (upper == "DEBUG")
        return LogLevel::Debug;
    if (upper == "INFO")
        return LogLevel::Info;
    if (upper == "WARN" || upper == "WARNING")
        return LogLevel::Warn;
    if (upper == "ERROR")
        return LogLevel::Error;
    if (upper == "OFF" || upper == "NONE")
        return LogLevel::Off;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// ConsoleLogSink
// ---------------------------------------------------------------------------

ConsoleLogSink::ConsoleLogSink(bool useJson, bool useColor) : json_(useJson), color_(useColor) {}

void ConsoleLogSink::write(const LogRecord& record) {
    const std::string line =
        json_ ? LogManager::toJson(record).dump() : LogManager::renderHuman(record, color_);
    // Logs go to stderr so that machine-readable report output on stdout stays
    // clean and pipeable.
    const std::lock_guard<std::mutex> lock(mutex_);
    std::fputs(line.c_str(), stderr);
    std::fputc('\n', stderr);
}

void ConsoleLogSink::flush() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// FileLogSink
// ---------------------------------------------------------------------------

FileLogSink::FileLogSink(const std::string& path) : path_(path) {
    stream_ = std::fopen(path.c_str(), "ae");  // append, close-on-exec
    if (stream_ == nullptr) {
        // Falling back silently would hide the problem; report once on stderr
        // and continue with logging disabled for this sink.
        std::fprintf(stderr, "[testforge] warning: cannot open log file '%s'\n", path.c_str());
    }
}

FileLogSink::~FileLogSink() {
    if (stream_ != nullptr) {
        std::fclose(stream_);
        stream_ = nullptr;
    }
}

void FileLogSink::write(const LogRecord& record) {
    if (stream_ == nullptr) {
        return;
    }
    const std::string line = LogManager::toJson(record).dump();
    const std::lock_guard<std::mutex> lock(mutex_);
    std::fputs(line.c_str(), stream_);
    std::fputc('\n', stream_);
}

void FileLogSink::flush() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stream_ != nullptr) {
        std::fflush(stream_);
    }
}

// ---------------------------------------------------------------------------
// MemoryLogSink
// ---------------------------------------------------------------------------

MemoryLogSink::MemoryLogSink(std::size_t capacity) : capacity_(capacity) {}

void MemoryLogSink::write(const LogRecord& record) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (records_.size() >= capacity_) {
        // Ring behaviour: keep the most recent records. Failure triage almost
        // always wants the tail, and an unbounded buffer is a memory leak in a
        // long-running server.
        records_.erase(records_.begin());
    }
    records_.push_back(record);
}

void MemoryLogSink::flush() {}

std::vector<LogRecord> MemoryLogSink::records() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return records_;
}

std::vector<std::string> MemoryLogSink::renderedLines() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> lines;
    lines.reserve(records_.size());
    for (const LogRecord& record : records_) {
        lines.push_back(LogManager::renderHuman(record, false));
    }
    return lines;
}

void MemoryLogSink::clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
}

// ---------------------------------------------------------------------------
// LogManager
// ---------------------------------------------------------------------------

LogManager& LogManager::instance() {
    // Function-local static: thread-safe initialisation guaranteed since
    // C++11, and no static initialisation order problem with other globals.
    static LogManager manager;
    return manager;
}

void LogManager::setLevel(LogLevel level) noexcept {
    level_.store(level, std::memory_order_relaxed);
}

LogLevel LogManager::level() const noexcept {
    return level_.load(std::memory_order_relaxed);
}

bool LogManager::enabled(LogLevel level) const noexcept {
    return level != LogLevel::Off && level >= level_.load(std::memory_order_relaxed);
}

void LogManager::addSink(std::shared_ptr<LogSink> sink) {
    if (!sink) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(std::move(sink));
}

std::vector<std::shared_ptr<LogSink>> LogManager::replaceSinks(
    std::vector<std::shared_ptr<LogSink>> sinks) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<LogSink>> previous = std::move(sinks_);
    sinks_ = std::move(sinks);
    return previous;
}

void LogManager::submit(const LogRecord& record) {
    // Copy the sink list under the lock, then write outside it. A slow sink
    // (a file on a busy disk) must not block other threads from logging, and
    // holding the lock across a virtual call invites deadlock if a sink ever
    // logs.
    std::vector<std::shared_ptr<LogSink>> sinks;
    std::string runId;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        sinks = sinks_;
        runId = runId_;
    }

    LogRecord stamped = record;
    if (stamped.runId.empty()) {
        stamped.runId = runId;
    }

    // Per-thread capture happens before the sinks so that a record is captured
    // even if a sink throws.
    for (ScopedLogCapture* capture : captureStack()) {
        if (capture->records_.size() < capture->capacity_) {
            capture->records_.push_back(stamped);
        }
    }

    for (const std::shared_ptr<LogSink>& sink : sinks) {
        sink->write(stamped);
    }
}

void LogManager::flush() {
    std::vector<std::shared_ptr<LogSink>> sinks;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        sinks = sinks_;
    }
    for (const std::shared_ptr<LogSink>& sink : sinks) {
        sink->flush();
    }
}

void LogManager::setRunId(std::string runId) {
    const std::lock_guard<std::mutex> lock(mutex_);
    runId_ = std::move(runId);
}

std::string LogManager::runId() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return runId_;
}

std::string LogManager::renderHuman(const LogRecord& record, bool color) {
    std::ostringstream os;
    os << toIso8601(record.timestamp) << ' ';
    if (color) {
        os << levelColor(record.level);
    }
    os << strings::padRight(toString(record.level), 5);
    if (color) {
        os << kAnsiReset;
    }
    os << " [" << record.component << ']';
    if (!record.test.empty()) {
        os << " {" << record.test << '}';
    }
    os << ' ' << record.message;

    for (const json::Member& field : record.fields) {
        os << ' ' << field.first << '=';
        if (field.second.isString()) {
            os << field.second.asString();
        } else {
            os << field.second.dump();
        }
    }
    return os.str();
}

json::Value LogManager::toJson(const LogRecord& record) {
    json::Value out = json::Value::object();
    out.set("timestamp", toIso8601(record.timestamp));
    out.set("level", std::string(toString(record.level)));
    out.set("component", record.component);
    out.set("message", record.message);
    if (!record.test.empty()) {
        out.set("test", record.test);
    }
    if (!record.runId.empty()) {
        out.set("run_id", record.runId);
    }
    out.set("thread", static_cast<std::int64_t>(record.threadId));
    if (!record.fields.empty()) {
        out.set("fields", json::Value(record.fields));
    }
    return out;
}

// ---------------------------------------------------------------------------
// ScopedLogCapture
// ---------------------------------------------------------------------------

ScopedLogCapture::ScopedLogCapture(std::size_t capacity) : capacity_(capacity) {
    records_.reserve(std::min<std::size_t>(capacity, 64));
    captureStack().push_back(this);
}

ScopedLogCapture::~ScopedLogCapture() {
    std::vector<ScopedLogCapture*>& stack = captureStack();
    // Normally this is the top of the stack; erase defensively in case a
    // caller destroyed captures out of order.
    const auto found = std::find(stack.rbegin(), stack.rend(), this);
    if (found != stack.rend()) {
        stack.erase(std::next(found).base());
    }
}

std::vector<std::string> ScopedLogCapture::lines() const {
    std::vector<std::string> out;
    out.reserve(records_.size());
    for (const LogRecord& record : records_) {
        out.push_back(LogManager::renderHuman(record, false));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

Logger Logger::forTest(std::string testName) const {
    Logger copy = *this;
    copy.test_ = std::move(testName);
    return copy;
}

Logger Logger::child(std::string_view suffix) const {
    Logger copy = *this;
    copy.component_ += '.';
    copy.component_.append(suffix);
    return copy;
}

void Logger::log(LogLevel level,
                 std::string_view message,
                 std::initializer_list<json::Member> fields) const {
    LogManager& manager = LogManager::instance();
    if (!manager.enabled(level)) {
        return;
    }

    LogRecord record;
    record.timestamp = WallClock::now();
    record.level = level;
    record.component = component_;
    // Every message passes through redaction on its way to a sink; this is the
    // single choke point that keeps credentials out of logs and reports.
    record.message = strings::redactSecrets(message);
    record.test = test_;
    record.threadId = currentThreadId();

    record.fields.reserve(fields.size());
    for (const json::Member& field : fields) {
        if (field.second.isString()) {
            record.fields.emplace_back(
                field.first, json::Value(strings::redactSecrets(field.second.asString())));
        } else {
            record.fields.push_back(field);
        }
    }

    manager.submit(record);
}

void Logger::trace(std::string_view message, std::initializer_list<json::Member> fields) const {
    log(LogLevel::Trace, message, fields);
}

void Logger::debug(std::string_view message, std::initializer_list<json::Member> fields) const {
    log(LogLevel::Debug, message, fields);
}

void Logger::info(std::string_view message, std::initializer_list<json::Member> fields) const {
    log(LogLevel::Info, message, fields);
}

void Logger::warn(std::string_view message, std::initializer_list<json::Member> fields) const {
    log(LogLevel::Warn, message, fields);
}

void Logger::error(std::string_view message, std::initializer_list<json::Member> fields) const {
    log(LogLevel::Error, message, fields);
}

}  // namespace testforge
