#include "testforge/core/Clock.hpp"

#include <array>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace testforge {
namespace {

/// Thread-safe gmtime. gmtime() returns a pointer to a shared static buffer,
/// which would be a data race the moment two workers format a timestamp
/// concurrently.
std::tm gmtimeSafe(std::time_t t) {
    std::tm out{};
#if defined(_WIN32)
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}

std::time_t timegmSafe(std::tm& tm) {
#if defined(_WIN32)
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

}  // namespace

std::string toIso8601(TimePoint tp) {
    const auto sinceEpoch = tp.time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch - seconds);

    const std::tm tm = gmtimeSafe(std::time_t{seconds.count()});

    std::array<char, 32> buffer{};
    const std::size_t written =
        std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S", &tm);
    if (written == 0) {
        return "1970-01-01T00:00:00.000Z";
    }

    std::ostringstream os;
    os << std::string(buffer.data(), written) << '.' << std::setfill('0') << std::setw(3)
       << millis.count() << 'Z';
    return os.str();
}

TimePoint fromIso8601(const std::string& text) {
    if (text.size() < 19) {
        return TimePoint{};
    }
    std::tm tm{};
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millis = 0;

    const int matched = std::sscanf(text.c_str(),
                                    "%4d-%2d-%2dT%2d:%2d:%2d.%3d",
                                    &year,
                                    &month,
                                    &day,
                                    &hour,
                                    &minute,
                                    &second,
                                    &millis);
    if (matched < 6) {
        return TimePoint{};
    }

    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = 0;

    const std::time_t epoch = timegmSafe(tm);
    if (epoch == static_cast<std::time_t>(-1)) {
        return TimePoint{};
    }
    return WallClock::from_time_t(epoch) + std::chrono::milliseconds(matched >= 7 ? millis : 0);
}

std::int64_t toEpochMillis(TimePoint tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

TimePoint fromEpochMillis(std::int64_t millis) {
    return TimePoint{std::chrono::milliseconds(millis)};
}

std::string formatDuration(Milliseconds duration) {
    const std::int64_t total = duration.count();
    if (total < 0) {
        return "0ms";
    }
    if (total < 1000) {
        return std::to_string(total) + "ms";
    }
    if (total < 60'000) {
        std::array<char, 32> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%.3fs", static_cast<double>(total) / 1000.0);
        return {buffer.data()};
    }
    const std::int64_t minutes = total / 60'000;
    const double seconds = static_cast<double>(total % 60'000) / 1000.0;
    std::array<char, 48> buffer{};
    std::snprintf(
        buffer.data(), buffer.size(), "%lldm %05.2fs", static_cast<long long>(minutes), seconds);
    return {buffer.data()};
}

}  // namespace testforge
