#include "testforge/core/Json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>

namespace testforge::json {
namespace {

/// Appends `cp` to `out` as UTF-8. Lone surrogates are replaced with U+FFFD
/// rather than producing invalid UTF-8 that would poison downstream consumers.
void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp >= 0xD800u && cp <= 0xDFFFu) {
        cp = 0xFFFDu;
    }
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

void escapeInto(std::string& out, std::string_view text) {
    static constexpr std::array<char, 17> kHex = {
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f', '\0'};
    out.push_back('"');
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (byte < 0x20u) {
                    out += "\\u00";
                    out.push_back(kHex[(byte >> 4) & 0xFu]);
                    out.push_back(kHex[byte & 0xFu]);
                } else {
                    // Bytes >= 0x20 pass through verbatim, which keeps valid
                    // UTF-8 input as valid UTF-8 output.
                    out.push_back(raw);
                }
                break;
        }
    }
    out.push_back('"');
}

/// Formats a double as valid JSON. Non-finite values have no JSON
/// representation; emitting null is the least surprising fallback and keeps
/// the output parseable.
std::string formatDouble(double v) {
    if (!std::isfinite(v)) {
        return "null";
    }
    std::array<char, 40> buffer{};
    // 17 significant digits round-trips any IEEE-754 double exactly.
    int written = std::snprintf(buffer.data(), buffer.size(), "%.17g", v);
    if (written <= 0) {
        return "0";
    }
    std::string text(buffer.data(), static_cast<std::size_t>(written));

    // %.17g is exact but ugly (0.10000000000000001). Prefer the shortest
    // representation that still round-trips.
    for (int precision = 1; precision < 17; ++precision) {
        written = std::snprintf(buffer.data(), buffer.size(), "%.*g", precision, v);
        if (written <= 0) {
            continue;
        }
        const std::string candidate(buffer.data(), static_cast<std::size_t>(written));
        if (std::strtod(candidate.c_str(), nullptr) == v) {
            text = candidate;
            break;
        }
    }

    // Ensure the result still reads as a floating point number so that a
    // round-trip does not silently change the JSON type.
    if (text.find_first_of(".eEn") == std::string::npos) {
        text += ".0";
    }
    return text;
}

class Parser {
 public:
    Parser(std::string_view text, const ParseLimits& limits) : text_(text), limits_(limits) {}

    Value parseDocument() {
        skipWhitespace();
        Value result = parseValue(0);
        skipWhitespace();
        if (pos_ != text_.size()) {
            fail("trailing characters after top-level value");
        }
        return result;
    }

 private:
    [[noreturn]] void fail(const std::string& message) const {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        throw ParseError(message, pos_, line, column);
    }

    [[nodiscard]] bool atEnd() const noexcept { return pos_ >= text_.size(); }

    [[nodiscard]] char peek() const {
        if (atEnd()) {
            return '\0';
        }
        return text_[pos_];
    }

    void skipWhitespace() {
        while (!atEnd()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    void expect(char c) {
        if (atEnd() || text_[pos_] != c) {
            fail(std::string("expected '") + c + "'");
        }
        ++pos_;
    }

    void consumeLiteral(std::string_view literal) {
        if (text_.substr(pos_, literal.size()) != literal) {
            fail("invalid literal");
        }
        pos_ += literal.size();
    }

    Value parseValue(std::size_t depth) {
        if (depth > limits_.maxDepth) {
            fail("maximum nesting depth exceeded");
        }
        if (atEnd()) {
            fail("unexpected end of input");
        }
        switch (peek()) {
            case 'n':
                consumeLiteral("null");
                return Value{};
            case 't':
                consumeLiteral("true");
                return Value{true};
            case 'f':
                consumeLiteral("false");
                return Value{false};
            case '"':
                return Value{parseString()};
            case '[':
                return parseArray(depth);
            case '{':
                return parseObject(depth);
            default:
                return parseNumber();
        }
    }

    Value parseArray(std::size_t depth) {
        expect('[');
        Array items;
        skipWhitespace();
        if (peek() == ']') {
            ++pos_;
            return Value{std::move(items)};
        }
        while (true) {
            skipWhitespace();
            items.push_back(parseValue(depth + 1));
            skipWhitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                break;
            }
            fail("expected ',' or ']' in array");
        }
        return Value{std::move(items)};
    }

    Value parseObject(std::size_t depth) {
        expect('{');
        Object members;
        skipWhitespace();
        if (peek() == '}') {
            ++pos_;
            return Value{std::move(members)};
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                fail("expected string key in object");
            }
            std::string key = parseString();
            skipWhitespace();
            expect(':');
            skipWhitespace();
            Value value = parseValue(depth + 1);

            // Last-one-wins on duplicate keys, matching the behaviour of every
            // mainstream JSON implementation.
            auto existing = std::find_if(
                members.begin(), members.end(), [&key](const Member& m) { return m.first == key; });
            if (existing != members.end()) {
                existing->second = std::move(value);
            } else {
                members.emplace_back(std::move(key), std::move(value));
            }

            skipWhitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                break;
            }
            fail("expected ',' or '}' in object");
        }
        return Value{std::move(members)};
    }

    std::uint32_t parseHex4() {
        if (pos_ + 4 > text_.size()) {
            fail("truncated \\u escape");
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4u;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                fail("invalid hex digit in \\u escape");
            }
        }
        return value;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (true) {
            if (atEnd()) {
                fail("unterminated string");
            }
            const char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                break;
            }
            if (c == '\\') {
                ++pos_;
                if (atEnd()) {
                    fail("unterminated escape sequence");
                }
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"':
                        out.push_back('"');
                        break;
                    case '\\':
                        out.push_back('\\');
                        break;
                    case '/':
                        out.push_back('/');
                        break;
                    case 'b':
                        out.push_back('\b');
                        break;
                    case 'f':
                        out.push_back('\f');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    case 'u': {
                        std::uint32_t cp = parseHex4();
                        // Recombine a UTF-16 surrogate pair into one code point.
                        if (cp >= 0xD800u && cp <= 0xDBFFu && pos_ + 1 < text_.size() &&
                            text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                            const std::size_t saved = pos_;
                            pos_ += 2;
                            const std::uint32_t low = parseHex4();
                            if (low >= 0xDC00u && low <= 0xDFFFu) {
                                cp = 0x10000u + ((cp - 0xD800u) << 10u) + (low - 0xDC00u);
                            } else {
                                pos_ = saved;  // not a valid pair; leave it alone
                            }
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default:
                        fail("invalid escape sequence");
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20u) {
                fail("unescaped control character in string");
            }
            out.push_back(c);
            ++pos_;
        }
        return out;
    }

    Value parseNumber() {
        const std::size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        if (atEnd()) {
            fail("truncated number");
        }
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        } else {
            fail("invalid number");
        }

        bool isDouble = false;
        if (!atEnd() && peek() == '.') {
            isDouble = true;
            ++pos_;
            if (atEnd() || peek() < '0' || peek() > '9') {
                fail("expected digit after decimal point");
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            isDouble = true;
            ++pos_;
            if (!atEnd() && (peek() == '+' || peek() == '-')) {
                ++pos_;
            }
            if (atEnd() || peek() < '0' || peek() > '9') {
                fail("expected digit in exponent");
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        }

        const std::string literal(text_.substr(start, pos_ - start));
        if (!isDouble) {
            errno = 0;
            char* end = nullptr;
            const long long parsed = std::strtoll(literal.c_str(), &end, 10);
            if (errno != ERANGE && end != nullptr && *end == '\0') {
                return Value{static_cast<std::int64_t>(parsed)};
            }
            // Integer literal too large for int64 — fall through to double so
            // that the document still parses, with documented precision loss.
        }
        return Value{std::strtod(literal.c_str(), nullptr)};
    }

    std::string_view text_;
    ParseLimits limits_;
    std::size_t pos_ = 0;
};

void dumpInto(std::string& out, const Value& value, int indent, int depth) {
    const bool pretty = indent >= 0;
    const std::string pad =
        pretty ? std::string(static_cast<std::size_t>(indent * (depth + 1)), ' ') : std::string();
    const std::string closePad =
        pretty ? std::string(static_cast<std::size_t>(indent * depth), ' ') : std::string();
    const char* newline = pretty ? "\n" : "";
    const char* colonSpace = pretty ? ": " : ":";

    switch (value.type()) {
        case Value::Type::Null:
            out += "null";
            break;
        case Value::Type::Boolean:
            out += value.asBool() ? "true" : "false";
            break;
        case Value::Type::Integer:
            out += std::to_string(value.asInt());
            break;
        case Value::Type::Double:
            out += formatDouble(value.asDouble());
            break;
        case Value::Type::String:
            escapeInto(out, value.asString());
            break;
        case Value::Type::Array: {
            const Array& items = value.asArray();
            if (items.empty()) {
                out += "[]";
                break;
            }
            out += '[';
            out += newline;
            for (std::size_t i = 0; i < items.size(); ++i) {
                out += pad;
                dumpInto(out, items[i], indent, depth + 1);
                if (i + 1 < items.size()) {
                    out += ',';
                }
                out += newline;
            }
            out += closePad;
            out += ']';
            break;
        }
        case Value::Type::Object: {
            const Object& members = value.asObject();
            if (members.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            out += newline;
            for (std::size_t i = 0; i < members.size(); ++i) {
                out += pad;
                escapeInto(out, members[i].first);
                out += colonSpace;
                dumpInto(out, members[i].second, indent, depth + 1);
                if (i + 1 < members.size()) {
                    out += ',';
                }
                out += newline;
            }
            out += closePad;
            out += '}';
            break;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// ParseError
// ---------------------------------------------------------------------------

namespace {
std::string formatParseError(const std::string& message, std::size_t line, std::size_t column) {
    std::ostringstream os;
    os << "JSON parse error at line " << line << ", column " << column << ": " << message;
    return os.str();
}
}  // namespace

ParseError::ParseError(std::string message,
                       std::size_t offset,
                       std::size_t line,
                       std::size_t column)
    : std::runtime_error(formatParseError(message, line, column)),
      offset_(offset),
      line_(line),
      column_(column) {}

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

Value::Type Value::type() const noexcept {
    return static_cast<Type>(data_.index());
}

std::string_view Value::typeName() const noexcept {
    switch (type()) {
        case Type::Null:
            return "null";
        case Type::Boolean:
            return "boolean";
        case Type::Integer:
            return "integer";
        case Type::Double:
            return "double";
        case Type::String:
            return "string";
        case Type::Array:
            return "array";
        case Type::Object:
            return "object";
    }
    return "unknown";
}

namespace {
[[noreturn]] void throwTypeError(std::string_view wanted, std::string_view got) {
    throw TypeError("JSON value is " + std::string(got) + ", expected " + std::string(wanted));
}
}  // namespace

bool Value::asBool() const {
    if (const auto* v = std::get_if<bool>(&data_)) {
        return *v;
    }
    throwTypeError("boolean", typeName());
}

std::int64_t Value::asInt() const {
    if (const auto* v = std::get_if<std::int64_t>(&data_)) {
        return *v;
    }
    if (const auto* d = std::get_if<double>(&data_)) {
        return static_cast<std::int64_t>(*d);
    }
    throwTypeError("integer", typeName());
}

double Value::asDouble() const {
    if (const auto* v = std::get_if<double>(&data_)) {
        return *v;
    }
    if (const auto* i = std::get_if<std::int64_t>(&data_)) {
        return static_cast<double>(*i);
    }
    throwTypeError("number", typeName());
}

const std::string& Value::asString() const {
    if (const auto* v = std::get_if<std::string>(&data_)) {
        return *v;
    }
    throwTypeError("string", typeName());
}

const Array& Value::asArray() const {
    if (const auto* v = std::get_if<Array>(&data_)) {
        return *v;
    }
    throwTypeError("array", typeName());
}

const Object& Value::asObject() const {
    if (const auto* v = std::get_if<Object>(&data_)) {
        return *v;
    }
    throwTypeError("object", typeName());
}

Array& Value::asArray() {
    if (auto* v = std::get_if<Array>(&data_)) {
        return *v;
    }
    throwTypeError("array", typeName());
}

Object& Value::asObject() {
    if (auto* v = std::get_if<Object>(&data_)) {
        return *v;
    }
    throwTypeError("object", typeName());
}

bool Value::boolOr(bool fallback) const noexcept {
    if (const auto* v = std::get_if<bool>(&data_)) {
        return *v;
    }
    return fallback;
}

std::int64_t Value::intOr(std::int64_t fallback) const noexcept {
    if (const auto* v = std::get_if<std::int64_t>(&data_)) {
        return *v;
    }
    if (const auto* d = std::get_if<double>(&data_)) {
        return static_cast<std::int64_t>(*d);
    }
    return fallback;
}

double Value::doubleOr(double fallback) const noexcept {
    if (const auto* v = std::get_if<double>(&data_)) {
        return *v;
    }
    if (const auto* i = std::get_if<std::int64_t>(&data_)) {
        return static_cast<double>(*i);
    }
    return fallback;
}

std::string Value::stringOr(std::string_view fallback) const {
    if (const auto* v = std::get_if<std::string>(&data_)) {
        return *v;
    }
    return std::string(fallback);
}

const Value* Value::find(std::string_view key) const noexcept {
    const auto* members = std::get_if<Object>(&data_);
    if (members == nullptr) {
        return nullptr;
    }
    for (const Member& member : *members) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

Value* Value::find(std::string_view key) noexcept {
    auto* members = std::get_if<Object>(&data_);
    if (members == nullptr) {
        return nullptr;
    }
    for (Member& member : *members) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

const Value& Value::at(std::string_view key) const {
    const Value* found = find(key);
    if (found == nullptr) {
        throw TypeError("JSON object has no member '" + std::string(key) + "'");
    }
    return *found;
}

const Value* Value::path(std::string_view dotted) const noexcept {
    const Value* current = this;
    std::size_t start = 0;
    while (start <= dotted.size()) {
        const std::size_t dot = dotted.find('.', start);
        const std::string_view segment = dotted.substr(
            start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (segment.empty()) {
            return nullptr;
        }
        current = current->find(segment);
        if (current == nullptr) {
            return nullptr;
        }
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return current;
}

Value& Value::set(std::string key, Value value) {
    if (std::holds_alternative<std::monostate>(data_)) {
        data_ = Object{};
    }
    auto* members = std::get_if<Object>(&data_);
    if (members == nullptr) {
        throwTypeError("object", typeName());
    }
    for (Member& member : *members) {
        if (member.first == key) {
            member.second = std::move(value);
            return *this;
        }
    }
    members->emplace_back(std::move(key), std::move(value));
    return *this;
}

Value& Value::push(Value value) {
    if (std::holds_alternative<std::monostate>(data_)) {
        data_ = Array{};
    }
    auto* items = std::get_if<Array>(&data_);
    if (items == nullptr) {
        throwTypeError("array", typeName());
    }
    items->push_back(std::move(value));
    return *this;
}

std::size_t Value::size() const noexcept {
    if (const auto* items = std::get_if<Array>(&data_)) {
        return items->size();
    }
    if (const auto* members = std::get_if<Object>(&data_)) {
        return members->size();
    }
    if (const auto* text = std::get_if<std::string>(&data_)) {
        return text->size();
    }
    return 0;
}

const Value& Value::operator[](std::size_t index) const {
    const Array& items = asArray();
    if (index >= items.size()) {
        throw TypeError("JSON array index out of range");
    }
    return items[index];
}

bool Value::operator==(const Value& other) const noexcept {
    // Integer and double compare numerically so that 1 == 1.0, which keeps
    // spec comparisons from failing on an irrelevant lexical difference.
    if (isNumber() && other.isNumber()) {
        if (isInteger() && other.isInteger()) {
            return asInt() == other.asInt();
        }
        return doubleOr(0.0) == other.doubleOr(0.0);
    }
    if (type() != other.type()) {
        return false;
    }
    switch (type()) {
        case Type::Null:
            return true;
        case Type::Boolean:
            return asBool() == other.asBool();
        case Type::String:
            return asString() == other.asString();
        case Type::Array: {
            const Array& a = asArray();
            const Array& b = other.asArray();
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
        }
        case Type::Object: {
            const Object& a = asObject();
            const Object& b = other.asObject();
            if (a.size() != b.size()) {
                return false;
            }
            // Order-insensitive: two objects with the same members are equal.
            for (const Member& member : a) {
                const Value* rhs = other.find(member.first);
                if (rhs == nullptr || !(member.second == *rhs)) {
                    return false;
                }
            }
            return true;
        }
        case Type::Integer:
        case Type::Double:
            break;
    }
    return false;
}

std::string Value::dump() const {
    std::string out;
    dumpInto(out, *this, -1, 0);
    return out;
}

std::string Value::dump(int indent) const {
    std::string out;
    dumpInto(out, *this, indent, 0);
    return out;
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

Value parse(std::string_view text, const ParseLimits& limits) {
    if (text.size() > limits.maxLength) {
        throw ParseError("document exceeds maximum length", 0, 1, 1);
    }
    Parser parser(text, limits);
    return parser.parseDocument();
}

std::optional<Value> tryParse(std::string_view text,
                              std::string* error,
                              const ParseLimits& limits) {
    try {
        return parse(text, limits);
    } catch (const ParseError& e) {
        if (error != nullptr) {
            *error = e.what();
        }
        return std::nullopt;
    }
}

std::string quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    escapeInto(out, text);
    return out;
}

std::string stringAt(const Value& value, std::string_view dottedPath, std::string_view fallback) {
    const Value* found = value.path(dottedPath);
    return found != nullptr ? found->stringOr(fallback) : std::string(fallback);
}

std::int64_t intAt(const Value& value, std::string_view dottedPath, std::int64_t fallback) {
    const Value* found = value.path(dottedPath);
    return found != nullptr ? found->intOr(fallback) : fallback;
}

double doubleAt(const Value& value, std::string_view dottedPath, double fallback) {
    const Value* found = value.path(dottedPath);
    return found != nullptr ? found->doubleOr(fallback) : fallback;
}

bool boolAt(const Value& value, std::string_view dottedPath, bool fallback) {
    const Value* found = value.path(dottedPath);
    return found != nullptr ? found->boolOr(fallback) : fallback;
}

const Array& arrayAt(const Value& value, std::string_view dottedPath) {
    // A function-local static so callers can iterate the result without a
    // null check and without the caller having to own an empty vector.
    static const Array kEmpty;
    const Value* found = value.path(dottedPath);
    return (found != nullptr && found->isArray()) ? found->asArray() : kEmpty;
}

}  // namespace testforge::json
