#include "testforge/core/Environment.hpp"

#include "testforge/core/StringUtils.hpp"

#include <array>
#include <cstdlib>
#include <fstream>

#if defined(_WIN32)
#include <cstdlib>
#else
extern char** environ;
#endif

namespace testforge::env {
namespace {

/// Variables worth attaching to a failure report. Anything not on this list is
/// omitted rather than redacted: a smaller, curated set is more useful than a
/// wall of text, and it removes the risk of leaking something unanticipated.
constexpr std::array<std::string_view, 16> kDiagnosticNames = {"PATH",
                                                               "HOME",
                                                               "USER",
                                                               "SHELL",
                                                               "LANG",
                                                               "LC_ALL",
                                                               "PWD",
                                                               "HOSTNAME",
                                                               "CI",
                                                               "GITHUB_ACTIONS",
                                                               "GITHUB_RUN_ID",
                                                               "container",
                                                               "CUDA_VISIBLE_DEVICES",
                                                               "NVIDIA_VISIBLE_DEVICES",
                                                               "TESTFORGE_CONFIG",
                                                               "TESTFORGE_ENV"};

}  // namespace

std::optional<std::string> get(std::string_view name) {
    const std::string key(name);
    const char* value = std::getenv(key.c_str());  // NOLINT(concurrency-mt-unsafe)
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

std::string getOr(std::string_view name, std::string_view fallback) {
    return get(name).value_or(std::string(fallback));
}

bool getBool(std::string_view name, bool fallback) {
    const std::optional<std::string> raw = get(name);
    if (!raw.has_value() || raw->empty()) {
        return fallback;
    }
    bool parsed = fallback;
    if (strings::parseBool(*raw, parsed)) {
        return parsed;
    }
    return fallback;
}

std::int64_t getInt(std::string_view name, std::int64_t fallback) {
    const std::optional<std::string> raw = get(name);
    if (!raw.has_value()) {
        return fallback;
    }
    std::int64_t parsed = fallback;
    if (strings::parseInt(*raw, parsed)) {
        return parsed;
    }
    return fallback;
}

bool isSet(std::string_view name) {
    return get(name).has_value();
}

std::map<std::string, std::string> snapshotRedacted() {
    std::map<std::string, std::string> out;
#if !defined(_WIN32)
    if (environ == nullptr) {
        return out;
    }
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string_view line(*entry);
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string key(line.substr(0, eq));
        const std::string_view value = line.substr(eq + 1);
        out[key] = strings::isSensitiveName(key) ? "***REDACTED***" : strings::redactSecrets(value);
    }
#endif
    return out;
}

std::map<std::string, std::string> diagnosticSubset() {
    std::map<std::string, std::string> out;
    for (const std::string_view name : kDiagnosticNames) {
        const std::optional<std::string> value = get(name);
        if (!value.has_value()) {
            continue;
        }
        const std::string key(name);
        out[key] =
            strings::isSensitiveName(key) ? "***REDACTED***" : strings::redactSecrets(*value);
    }
    return out;
}

std::optional<std::size_t> loadDotEnv(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }

    std::size_t applied = 0;
    std::string line;
    while (std::getline(file, line)) {
        const std::string trimmed = strings::trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        // Tolerate the "export KEY=VALUE" form that people paste from shells.
        std::string body = trimmed;
        if (strings::startsWith(body, "export ")) {
            body = strings::trim(body.substr(7));
        }

        const std::size_t eq = body.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = strings::trim(body.substr(0, eq));
        std::string value = strings::trim(body.substr(eq + 1));
        if (key.empty()) {
            continue;
        }
        // Strip one layer of matching quotes.
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                  (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        if (isSet(key)) {
            continue;  // the real environment always wins
        }
#if defined(_WIN32)
        if (_putenv_s(key.c_str(), value.c_str()) == 0) {
            ++applied;
        }
#else
        if (setenv(key.c_str(), value.c_str(), 0) == 0) {  // NOLINT(concurrency-mt-unsafe)
            ++applied;
        }
#endif
    }
    return applied;
}

}  // namespace testforge::env
