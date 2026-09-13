#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace testforge {

/// Wall-clock instant, used for anything a human or a database will read.
using WallClock = std::chrono::system_clock;
using TimePoint = WallClock::time_point;

/// Monotonic clock, used for every duration measurement. steady_clock is
/// immune to NTP steps and daylight-saving jumps, which would otherwise show
/// up as negative or absurd test durations.
using SteadyClock = std::chrono::steady_clock;

using Milliseconds = std::chrono::milliseconds;
using Microseconds = std::chrono::microseconds;

/// A duration as a plain 64-bit integer, for JSON payloads and SQL columns.
///
/// One helper rather than a static_cast at every call site: chrono's `rep` is
/// implementation-defined, so the conversion belongs in a single documented
/// place. Braced initialisation is used deliberately — it is a compile error
/// if `rep` ever becomes a type that would narrow here, which a cast would
/// silently accept.
constexpr std::int64_t millisOf(Milliseconds duration) noexcept {
    return std::int64_t{duration.count()};
}

constexpr std::int64_t microsOf(Microseconds duration) noexcept {
    return std::int64_t{duration.count()};
}

/// ISO-8601 UTC with millisecond precision: "2026-09-12T06:31:32.417Z".
std::string toIso8601(TimePoint tp);

/// Parses the format produced by toIso8601. Returns the epoch on failure,
/// which the callers treat as "unknown timestamp".
TimePoint fromIso8601(const std::string& text);

/// Milliseconds since the Unix epoch — the storage format used in SQLite,
/// because integer comparison makes range queries trivial and index-friendly.
std::int64_t toEpochMillis(TimePoint tp);

TimePoint fromEpochMillis(std::int64_t millis);

/// "1.234s", "412ms", "3m 04s" — compact human rendering for reports.
std::string formatDuration(Milliseconds duration);

/// RAII duration measurement.
class Stopwatch {
 public:
    Stopwatch() : start_(SteadyClock::now()) {}

    void reset() noexcept { start_ = SteadyClock::now(); }

    [[nodiscard]] Milliseconds elapsed() const noexcept {
        return std::chrono::duration_cast<Milliseconds>(SteadyClock::now() - start_);
    }

    [[nodiscard]] double elapsedSeconds() const noexcept {
        return std::chrono::duration<double>(SteadyClock::now() - start_).count();
    }

    [[nodiscard]] Microseconds elapsedMicros() const noexcept {
        return std::chrono::duration_cast<Microseconds>(SteadyClock::now() - start_);
    }

 private:
    SteadyClock::time_point start_;
};

}  // namespace testforge
