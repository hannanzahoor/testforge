#pragma once

#include "testforge/core/Json.hpp"

#include <ostream>
#include <string>
#include <vector>

namespace testforge::cli {

/// Small terminal-formatting helpers shared by the command handlers.
class CliOutput {
 public:
    /// True when stdout is a terminal, so colour and progress make sense.
    static bool stdoutIsTerminal();

    static void setColorEnabled(bool enabled);

    static bool colorEnabled();

    static std::string bold(std::string_view text);

    static std::string dim(std::string_view text);

    static std::string green(std::string_view text);

    static std::string red(std::string_view text);

    static std::string yellow(std::string_view text);

    static std::string cyan(std::string_view text);

    /// Renders a table with aligned columns. `alignRight` marks numeric
    /// columns so figures line up on their last digit.
    static void table(std::ostream& out,
                      const std::vector<std::string>& headers,
                      const std::vector<std::vector<std::string>>& rows,
                      const std::vector<bool>& alignRight = {});

    static void heading(std::ostream& out, std::string_view text);

    static void keyValue(std::ostream& out,
                         std::string_view key,
                         std::string_view value,
                         std::size_t keyWidth = 20);

    /// Prints an error the way a good CLI does: to stderr, prefixed, and with
    /// a suggestion when one is available.
    static void error(std::string_view message, std::string_view hint = {});

    static void warning(std::string_view message);

    /// Pretty-prints JSON to stdout, for the --json flag.
    static void json(std::ostream& out, const json::Value& value);
};

}  // namespace testforge::cli
