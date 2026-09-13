#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace testforge::strings {

std::string trim(std::string_view text);

std::string toLower(std::string_view text);

std::string toUpper(std::string_view text);

bool startsWith(std::string_view text, std::string_view prefix) noexcept;

bool endsWith(std::string_view text, std::string_view suffix) noexcept;

bool contains(std::string_view text, std::string_view needle) noexcept;

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept;

bool containsIgnoreCase(std::string_view text, std::string_view needle) noexcept;

/// Splits on a single delimiter. Empty fields are preserved unless
/// `skipEmpty` is set, which is what CLI list options want.
std::vector<std::string> split(std::string_view text, char delimiter, bool skipEmpty = false);

std::vector<std::string> splitLines(std::string_view text);

std::string join(const std::vector<std::string>& parts, std::string_view separator);

std::string replaceAll(std::string_view text, std::string_view from, std::string_view to);

/// Truncates to `maxLength`, appending an ellipsis marker. Used to keep log
/// records and database columns bounded when a response body is large.
std::string truncate(std::string_view text, std::size_t maxLength);

/// Glob matching supporting '*' and '?'. Used by --test and --suite filters,
/// deliberately instead of std::regex: it is what users expect from a CLI and
/// it cannot catastrophically backtrack on hostile input.
bool globMatch(std::string_view pattern, std::string_view text) noexcept;

/// Masks anything that looks like a credential. Applied to every string that
/// reaches a log sink, a report, or the AI provider.
///
/// This is defence in depth, not a guarantee: it recognises common shapes
/// (Bearer tokens, sk-* API keys, key=value pairs with sensitive names) and
/// nothing else.
std::string redactSecrets(std::string_view text);

/// True when the name looks like it holds a credential ("token", "secret",
/// "password", "api_key", "authorization", ...).
bool isSensitiveName(std::string_view name) noexcept;

/// Escapes the five XML metacharacters. Used by the JUnit reporter.
std::string escapeXml(std::string_view text);

/// Escapes for embedding inside an HTML text node or attribute.
std::string escapeHtml(std::string_view text);

/// Lowercase hexadecimal encoding.
std::string toHex(std::string_view bytes);

/// Left-pads with spaces to `width` (no truncation).
std::string padRight(std::string_view text, std::size_t width);

std::string padLeft(std::string_view text, std::size_t width);

/// Parses "12", "1500ms", "2s", "3m" into milliseconds. Returns false when the
/// text is not a valid duration.
bool parseDurationMillis(std::string_view text, std::int64_t& outMillis) noexcept;

/// Strict integer parse (no partial matches, no leading/trailing junk).
bool parseInt(std::string_view text, std::int64_t& out) noexcept;

bool parseDouble(std::string_view text, double& out) noexcept;

bool parseBool(std::string_view text, bool& out) noexcept;

}  // namespace testforge::strings
