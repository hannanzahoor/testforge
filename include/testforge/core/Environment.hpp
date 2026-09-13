#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace testforge::env {

/// Reads an environment variable. Returns nullopt when unset (as opposed to
/// set-but-empty, which returns an empty string — the distinction matters for
/// flags such as TESTFORGE_AI_ENABLED="").
std::optional<std::string> get(std::string_view name);

std::string getOr(std::string_view name, std::string_view fallback);

bool getBool(std::string_view name, bool fallback);

std::int64_t getInt(std::string_view name, std::int64_t fallback);

bool isSet(std::string_view name);

/// Every environment variable, with the value of anything whose *name* looks
/// like a credential replaced by "***REDACTED***".
///
/// This is the only function that should ever be used to put the environment
/// into a report, a log, or an AI prompt.
std::map<std::string, std::string> snapshotRedacted();

/// The small, curated subset of variables that are actually useful for
/// diagnosing a test failure (PATH, HOME, CI, LANG, ...). Values still pass
/// through redaction.
std::map<std::string, std::string> diagnosticSubset();

/// Loads KEY=VALUE lines from a .env-style file into the process environment.
///
/// Existing variables are never overwritten: the real environment always wins
/// over a file, so `OPENAI_API_KEY=... testforge ...` behaves as expected.
/// Lines beginning with '#' and blank lines are ignored. Returns the number of
/// variables set, or nullopt when the file cannot be read.
std::optional<std::size_t> loadDotEnv(const std::string& path);

}  // namespace testforge::env
