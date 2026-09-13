#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace testforge::strings {
namespace {

char lowerAscii(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

char upperAscii(char c) noexcept {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

bool isSpace(char c) noexcept {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

/// Where a "key: value" run stops. Written as a function so the character set
/// is stated once and cannot drift between the rules that use it.
bool isValueTerminator(char c) noexcept {
    return c == '"' || c == ',' || c == '\n' || c == '\r' || c == '}' || c == '&';
}

/// What a redacted value is replaced with. Also used as the "already masked"
/// sentinel, which is what makes redaction idempotent.
constexpr std::string_view kRedactionMask = "***REDACTED***";

}  // namespace

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(text[end - 1])) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string toLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), lowerAscii);
    return out;
}

std::string toUpper(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), upperAscii);
    return out;
}

bool startsWith(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(std::string_view text, std::string_view needle) noexcept {
    return text.find(needle) != std::string_view::npos;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

bool containsIgnoreCase(std::string_view text, std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > text.size()) {
        return false;
    }
    const std::size_t limit = text.size() - needle.size();
    for (std::size_t i = 0; i <= limit; ++i) {
        if (equalsIgnoreCase(text.substr(i, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> split(std::string_view text, char delimiter, bool skipEmpty) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = text.find(delimiter, start);
        std::string_view piece =
            (pos == std::string_view::npos) ? text.substr(start) : text.substr(start, pos - start);
        if (!skipEmpty || !piece.empty()) {
            parts.emplace_back(piece);
        }
        if (pos == std::string_view::npos) {
            break;
        }
        start = pos + 1;
    }
    return parts;
}

std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> lines = split(text, '\n', false);
    for (std::string& line : lines) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
    }
    if (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out.append(separator);
        }
        out.append(parts[i]);
    }
    return out;
}

std::string replaceAll(std::string_view text, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return std::string(text);
    }
    std::string out;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = text.find(from, start);
        if (pos == std::string_view::npos) {
            out.append(text.substr(start));
            break;
        }
        out.append(text.substr(start, pos - start));
        out.append(to);
        start = pos + from.size();
    }
    return out;
}

std::string truncate(std::string_view text, std::size_t maxLength) {
    if (text.size() <= maxLength) {
        return std::string(text);
    }
    static constexpr std::string_view kMarker = "... [truncated]";
    if (maxLength <= kMarker.size()) {
        return std::string(text.substr(0, maxLength));
    }
    std::string out(text.substr(0, maxLength - kMarker.size()));
    out.append(kMarker);
    return out;
}

bool globMatch(std::string_view pattern, std::string_view text) noexcept {
    // Iterative backtracking match: linear in the common case and, unlike a
    // recursive implementation, cannot blow the stack on a hostile pattern
    // such as "*a*a*a*a*a*b".
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t starPattern = std::string_view::npos;
    std::size_t starText = 0;

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            starPattern = p++;
            starText = t;
        } else if (starPattern != std::string_view::npos) {
            p = starPattern + 1;
            t = ++starText;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

bool isSensitiveName(std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 12> kMarkers = {"password",
                                                                  "passwd",
                                                                  "secret",
                                                                  "token",
                                                                  "api_key",
                                                                  "apikey",
                                                                  "auth",
                                                                  "cookie",
                                                                  "session",
                                                                  "credential",
                                                                  "private",
                                                                  "signature"};
    for (const std::string_view marker : kMarkers) {
        if (containsIgnoreCase(name, marker)) {
            return true;
        }
    }
    return false;
}

std::string redactSecrets(std::string_view text) {
    // Three rules, applied in order, each of them iterative and idempotent.
    //
    // Idempotence is not a nicety here: redaction runs on every log record and
    // every report field, so the same string is frequently passed through more
    // than once. An earlier version of this function recursed after each
    // substitution and looped forever the second time it saw its own mask,
    // which took the process down with a stack overflow.
    std::string out(text);

    // --- 1. key: value / key=value, where the key names a credential -------
    //
    // Runs first, and masks the whole value rather than the first token, so
    // "Authorization: Bearer abc123" collapses to one mask instead of two.
    static constexpr std::array<std::string_view, 7> kKeys = {
        "authorization", "password", "passwd", "secret", "token", "api_key", "apikey"};

    for (const std::string_view key : kKeys) {
        std::size_t from = 0;
        while (true) {
            const std::string lowered = toLower(out);
            const std::size_t found = lowered.find(key, from);
            if (found == std::string::npos) {
                break;
            }

            std::size_t cursor = found + key.size();
            // Tolerate a closing quote and whitespace before the separator.
            while (cursor < out.size() && (out[cursor] == '"' || isSpace(out[cursor]))) {
                ++cursor;
            }
            if (cursor >= out.size() || (out[cursor] != ':' && out[cursor] != '=')) {
                from = found + key.size();
                continue;
            }
            ++cursor;
            while (cursor < out.size() && (isSpace(out[cursor]) || out[cursor] == '"')) {
                ++cursor;
            }

            std::size_t valueEnd = cursor;
            while (valueEnd < out.size() && !isValueTerminator(out[valueEnd])) {
                ++valueEnd;
            }
            // Trim trailing whitespace so the mask lands tightly on the value.
            while (valueEnd > cursor && isSpace(out[valueEnd - 1])) {
                --valueEnd;
            }

            if (valueEnd <= cursor) {
                from = cursor;
                continue;
            }
            if (out.compare(cursor, valueEnd - cursor, kRedactionMask) == 0) {
                from = cursor + kRedactionMask.size();  // already masked
                continue;
            }
            out.replace(cursor, valueEnd - cursor, kRedactionMask);
            from = cursor + kRedactionMask.size();
        }
    }

    // --- 2. a bare "Bearer <token>" with no key in front of it -------------
    {
        std::size_t from = 0;
        while (true) {
            const std::string lowered = toLower(out);
            const std::size_t found = lowered.find("bearer ", from);
            if (found == std::string::npos) {
                break;
            }
            const std::size_t valueStart = found + 7;
            std::size_t valueEnd = valueStart;
            while (valueEnd < out.size() && !isSpace(out[valueEnd]) && out[valueEnd] != '"' &&
                   out[valueEnd] != ',') {
                ++valueEnd;
            }
            if (valueEnd <= valueStart) {
                from = valueStart;
                continue;
            }
            if (out.compare(valueStart, valueEnd - valueStart, kRedactionMask) == 0) {
                from = valueStart + kRedactionMask.size();
                continue;
            }
            out.replace(valueStart, valueEnd - valueStart, kRedactionMask);
            from = valueStart + kRedactionMask.size();
        }
    }

    // --- 3. tokens recognisable by their own prefix ------------------------
    //
    // These are shapes published by the providers themselves, so they can be
    // spotted without any surrounding context. The length floor avoids masking
    // an innocent word that happens to start with "sk-".
    static constexpr std::array<std::string_view, 5> kPrefixes = {
        "sk-", "ghp_", "gho_", "xoxb-", "AKIA"};
    for (const std::string_view prefix : kPrefixes) {
        std::size_t from = 0;
        while (true) {
            const std::size_t found = out.find(prefix, from);
            if (found == std::string::npos) {
                break;
            }
            std::size_t valueEnd = found + prefix.size();
            while (valueEnd < out.size() &&
                   (std::isalnum(static_cast<unsigned char>(out[valueEnd])) != 0 ||
                    out[valueEnd] == '-' || out[valueEnd] == '_')) {
                ++valueEnd;
            }
            const std::size_t tailLength = valueEnd - (found + prefix.size());
            if (tailLength < 8) {
                from = valueEnd > found ? valueEnd : found + 1;
                continue;
            }
            out.replace(found, valueEnd - found, std::string(prefix) + std::string(kRedactionMask));
            from = found + prefix.size() + kRedactionMask.size();
        }
    }

    return out;
}

std::string escapeXml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                // XML 1.0 forbids most control characters outright.
                if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\r' && c != '\t') {
                    out += ' ';
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string escapeHtml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            case '/':
                out += "&#47;";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

std::string toHex(std::string_view bytes) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(kDigits[byte >> 4u]);
        out.push_back(kDigits[byte & 0x0Fu]);
    }
    return out;
}

std::string padRight(std::string_view text, std::size_t width) {
    std::string out(text);
    if (out.size() < width) {
        out.append(width - out.size(), ' ');
    }
    return out;
}

std::string padLeft(std::string_view text, std::size_t width) {
    std::string out;
    if (text.size() < width) {
        out.append(width - text.size(), ' ');
    }
    out.append(text);
    return out;
}

bool parseInt(std::string_view text, std::int64_t& out) noexcept {
    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(trimmed.c_str(), &end, 10);
    if (errno == ERANGE || end == nullptr || *end != '\0') {
        return false;
    }
    out = static_cast<std::int64_t>(parsed);
    return true;
}

bool parseDouble(std::string_view text, double& out) noexcept {
    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(trimmed.c_str(), &end);
    if (errno == ERANGE || end == nullptr || *end != '\0') {
        return false;
    }
    out = parsed;
    return true;
}

bool parseBool(std::string_view text, bool& out) noexcept {
    const std::string lowered = toLower(trim(text));
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        out = true;
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parseDurationMillis(std::string_view text, std::int64_t& outMillis) noexcept {
    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        return false;
    }
    std::size_t suffixStart = trimmed.size();
    while (suffixStart > 0 &&
           (std::isalpha(static_cast<unsigned char>(trimmed[suffixStart - 1])) != 0)) {
        --suffixStart;
    }
    const std::string number = trimmed.substr(0, suffixStart);
    const std::string suffix = toLower(trimmed.substr(suffixStart));

    double value = 0.0;
    if (!parseDouble(number, value) || value < 0.0) {
        return false;
    }

    double multiplier = 1.0;
    if (suffix.empty() || suffix == "ms") {
        multiplier = 1.0;
    } else if (suffix == "s" || suffix == "sec") {
        multiplier = 1000.0;
    } else if (suffix == "m" || suffix == "min") {
        multiplier = 60.0 * 1000.0;
    } else if (suffix == "h") {
        multiplier = 60.0 * 60.0 * 1000.0;
    } else {
        return false;
    }

    outMillis = static_cast<std::int64_t>(value * multiplier);
    return true;
}

}  // namespace testforge::strings
