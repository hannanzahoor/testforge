#pragma once

#include "testforge/testing/TestSelector.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace testforge::cli {

/// Result of parsing argv.
///
/// Options are kept as lists because repetition is meaningful:
/// `--tag api --tag smoke` selects both.
struct ParsedCommand {
    std::string command;     ///< "run", "list", ...
    std::string subcommand;  ///< "generate-tests" for `ai generate-tests`
    std::map<std::string, std::vector<std::string>> options;
    std::vector<std::string> positionals;

    bool helpRequested = false;
    bool versionRequested = false;

    /// Set when parsing failed; the message is ready to print.
    std::optional<std::string> error;

    [[nodiscard]] bool has(std::string_view name) const;

    [[nodiscard]] std::string value(std::string_view name, std::string_view fallback = {}) const;

    [[nodiscard]] std::vector<std::string> values(std::string_view name) const;

    [[nodiscard]] int intValue(std::string_view name, int fallback) const;

    [[nodiscard]] std::int64_t int64Value(std::string_view name, std::int64_t fallback) const;

    [[nodiscard]] bool flag(std::string_view name, bool fallback = false) const;
};

/// Parses `--flag`, `--option value`, `--option=value`, `-h`, and positionals.
///
/// Hand-written rather than pulled from a library: the grammar is small, and
/// a dependency for argument parsing would be the largest thing in the build
/// for the least benefit. Unknown options are an error, not a warning — a
/// typo in `--sutie smoke` must not silently run the whole suite.
class CommandLine {
 public:
    static ParsedCommand parse(int argc, const char* const* argv);

    /// Builds a TestFilter from the standard selection options.
    static TestFilter filterFrom(const ParsedCommand& parsed);

    static SelectionOptions selectionFrom(const ParsedCommand& parsed);

    /// Every option a command accepts, used for validation and for --help.
    [[nodiscard]] static const std::vector<std::string>& knownOptions(std::string_view command);

    [[nodiscard]] static const std::vector<std::string>& commands();
};

}  // namespace testforge::cli
