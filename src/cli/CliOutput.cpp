#include "CliOutput.hpp"

#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iostream>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace testforge::cli {
namespace {

std::atomic<bool> gColorEnabled{true};

std::string wrap(std::string_view code, std::string_view text) {
    if (!gColorEnabled.load(std::memory_order_relaxed)) {
        return std::string(text);
    }
    return std::string(code) + std::string(text) + "\033[0m";
}

}  // namespace

bool CliOutput::stdoutIsTerminal() {
#if defined(_WIN32)
    return false;
#else
    return ::isatty(STDOUT_FILENO) == 1;
#endif
}

void CliOutput::setColorEnabled(bool enabled) {
    gColorEnabled.store(enabled, std::memory_order_relaxed);
}

bool CliOutput::colorEnabled() {
    return gColorEnabled.load(std::memory_order_relaxed);
}

std::string CliOutput::bold(std::string_view text) {
    return wrap("\033[1m", text);
}

std::string CliOutput::dim(std::string_view text) {
    return wrap("\033[2m", text);
}

std::string CliOutput::green(std::string_view text) {
    return wrap("\033[32m", text);
}

std::string CliOutput::red(std::string_view text) {
    return wrap("\033[31m", text);
}

std::string CliOutput::yellow(std::string_view text) {
    return wrap("\033[33m", text);
}

std::string CliOutput::cyan(std::string_view text) {
    return wrap("\033[36m", text);
}

void CliOutput::table(std::ostream& out,
                      const std::vector<std::string>& headers,
                      const std::vector<std::vector<std::string>>& rows,
                      const std::vector<bool>& alignRight) {
    if (headers.empty()) {
        return;
    }

    std::vector<std::size_t> widths(headers.size());
    for (std::size_t i = 0; i < headers.size(); ++i) {
        widths[i] = headers[i].size();
    }
    for (const std::vector<std::string>& row : rows) {
        for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }

    const auto isRight = [&alignRight](std::size_t column) {
        return column < alignRight.size() && alignRight[column];
    };

    for (std::size_t i = 0; i < headers.size(); ++i) {
        const std::string cell = isRight(i) ? strings::padLeft(headers[i], widths[i])
                                            : strings::padRight(headers[i], widths[i]);
        out << bold(cell);
        if (i + 1 < headers.size()) {
            out << "  ";
        }
    }
    out << '\n';

    for (std::size_t i = 0; i < headers.size(); ++i) {
        out << dim(std::string(widths[i], '-'));
        if (i + 1 < headers.size()) {
            out << "  ";
        }
    }
    out << '\n';

    for (const std::vector<std::string>& row : rows) {
        for (std::size_t i = 0; i < widths.size(); ++i) {
            const std::string value = i < row.size() ? row[i] : std::string{};
            // The last column is not padded: trailing spaces are just noise
            // when the output is piped somewhere.
            if (i + 1 == widths.size()) {
                out << (isRight(i) ? strings::padLeft(value, widths[i]) : value);
            } else {
                out << (isRight(i) ? strings::padLeft(value, widths[i])
                                   : strings::padRight(value, widths[i]))
                    << "  ";
            }
        }
        out << '\n';
    }
}

void CliOutput::heading(std::ostream& out, std::string_view text) {
    out << '\n'
        << bold(text) << '\n'
        << dim(std::string(std::min<std::size_t>(text.size(), 72), '-')) << '\n';
}

void CliOutput::keyValue(std::ostream& out,
                         std::string_view key,
                         std::string_view value,
                         std::size_t keyWidth) {
    out << "  " << dim(strings::padRight(key, keyWidth)) << value << '\n';
}

void CliOutput::error(std::string_view message, std::string_view hint) {
    std::cerr << red("error: ") << message << '\n';
    if (!hint.empty()) {
        std::cerr << dim("hint:  ") << hint << '\n';
    }
}

void CliOutput::warning(std::string_view message) {
    std::cerr << yellow("warning: ") << message << '\n';
}

void CliOutput::json(std::ostream& out, const json::Value& value) {
    out << value.dump(2) << '\n';
}

}  // namespace testforge::cli
