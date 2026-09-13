#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace testforge::json {

class Value;

// The public names Array, Member and Object are declared after Value, below.
// They cannot be declared here: Value::Type has enumerators called Array and
// Object, and a namespace-scope name of the same spelling declared first would
// be shadowed by them (-Wshadow), for no benefit.

/// Thrown by parse() when the input is not valid JSON, or exceeds a limit.
class ParseError : public std::runtime_error {
 public:
    ParseError(std::string message, std::size_t offset, std::size_t line, std::size_t column);

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

    [[nodiscard]] std::size_t line() const noexcept { return line_; }

    [[nodiscard]] std::size_t column() const noexcept { return column_; }

 private:
    std::size_t offset_;
    std::size_t line_;
    std::size_t column_;
};

/// Thrown when a Value is accessed as the wrong type.
class TypeError : public std::runtime_error {
 public:
    explicit TypeError(const std::string& message) : std::runtime_error(message) {}
};

/// A JSON value.
///
/// Integers and doubles are stored in distinct alternatives so that 64-bit
/// identifiers and epoch timestamps round-trip exactly rather than degrading
/// through a double.
class Value {
 public:
    enum class Type : std::uint8_t { Null, Boolean, Integer, Double, String, Array, Object };

    /// JSON arrays are plain vectors. std::vector is explicitly permitted to
    /// be instantiated with an incomplete element type, which is what makes
    /// this recursive definition legal.
    using ArrayStorage = std::vector<Value>;

    /// Objects preserve insertion order.
    ///
    /// A vector of pairs rather than std::map: JSON documents in TestForge are
    /// small (test specs, results, config) so linear lookup is irrelevant,
    /// while stable key ordering makes serialised output byte-for-byte
    /// reproducible. That matters because reports are diffed and hashed in CI.
    using MemberStorage = std::pair<std::string, Value>;
    using ObjectStorage = std::vector<MemberStorage>;

    Value() noexcept : data_(std::monostate{}) {}

    Value(std::nullptr_t) noexcept
        : data_(std::monostate{}) {}  // NOLINT(google-explicit-constructor)

    Value(bool v) noexcept : data_(v) {}  // NOLINT(google-explicit-constructor)

    Value(int v) noexcept : data_(static_cast<std::int64_t>(v)) {}  // NOLINT

    Value(long v) noexcept : data_(std::int64_t{v}) {}  // NOLINT

    Value(long long v) noexcept : data_(std::int64_t{v}) {}  // NOLINT

    Value(unsigned v) noexcept : data_(static_cast<std::int64_t>(v)) {}  // NOLINT

    Value(unsigned long v) noexcept : data_(static_cast<std::int64_t>(v)) {}  // NOLINT

    Value(unsigned long long v) noexcept  // NOLINT
        : data_(static_cast<std::int64_t>(v)) {}

    Value(double v) noexcept : data_(v) {}  // NOLINT(google-explicit-constructor)

    Value(const char* v) : data_(std::string(v != nullptr ? v : "")) {}  // NOLINT

    Value(std::string v) : data_(std::move(v)) {}  // NOLINT(google-explicit-constructor)

    Value(std::string_view v) : data_(std::string(v)) {}  // NOLINT

    Value(ArrayStorage v) : data_(std::move(v)) {}  // NOLINT(google-explicit-constructor)

    Value(ObjectStorage v) : data_(std::move(v)) {}  // NOLINT(google-explicit-constructor)

    static Value array() { return Value(ArrayStorage{}); }

    static Value object() { return Value(ObjectStorage{}); }

    static Value array(std::initializer_list<Value> items) { return Value(ArrayStorage(items)); }

    static Value object(std::initializer_list<MemberStorage> members) {
        return Value(ObjectStorage(members));
    }

    [[nodiscard]] Type type() const noexcept;

    [[nodiscard]] std::string_view typeName() const noexcept;

    [[nodiscard]] bool isNull() const noexcept { return type() == Type::Null; }

    [[nodiscard]] bool isBool() const noexcept { return type() == Type::Boolean; }

    [[nodiscard]] bool isInteger() const noexcept { return type() == Type::Integer; }

    [[nodiscard]] bool isDouble() const noexcept { return type() == Type::Double; }

    [[nodiscard]] bool isNumber() const noexcept { return isInteger() || isDouble(); }

    [[nodiscard]] bool isString() const noexcept { return type() == Type::String; }

    [[nodiscard]] bool isArray() const noexcept { return type() == Type::Array; }

    [[nodiscard]] bool isObject() const noexcept { return type() == Type::Object; }

    // --- checked accessors: throw TypeError on mismatch ---------------------
    [[nodiscard]] bool asBool() const;
    [[nodiscard]] std::int64_t asInt() const;
    [[nodiscard]] double asDouble() const;
    [[nodiscard]] const std::string& asString() const;
    [[nodiscard]] const ArrayStorage& asArray() const;
    [[nodiscard]] const ObjectStorage& asObject() const;
    [[nodiscard]] ArrayStorage& asArray();
    [[nodiscard]] ObjectStorage& asObject();

    // --- forgiving accessors: return the fallback on mismatch ---------------
    [[nodiscard]] bool boolOr(bool fallback) const noexcept;
    [[nodiscard]] std::int64_t intOr(std::int64_t fallback) const noexcept;
    [[nodiscard]] double doubleOr(double fallback) const noexcept;
    [[nodiscard]] std::string stringOr(std::string_view fallback) const;

    /// Object member lookup. Returns nullptr when absent, or when *this is not
    /// an object — callers that require the key should use at().
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    [[nodiscard]] Value* find(std::string_view key) noexcept;

    [[nodiscard]] bool contains(std::string_view key) const noexcept {
        return find(key) != nullptr;
    }

    /// Throws TypeError if absent.
    [[nodiscard]] const Value& at(std::string_view key) const;

    /// Dotted path lookup: "response.body.id". Returns nullptr if any segment
    /// is missing. Array indices are not supported — keep specs flat.
    [[nodiscard]] const Value* path(std::string_view dotted) const noexcept;

    /// Inserts or replaces an object member. Converts *this to an object if it
    /// is currently null; throws TypeError for any other type.
    Value& set(std::string key, Value value);

    /// Appends to an array. Converts *this to an array if it is currently
    /// null; throws TypeError for any other type.
    Value& push(Value value);

    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] const Value& operator[](std::size_t index) const;

    bool operator==(const Value& other) const noexcept;

    bool operator!=(const Value& other) const noexcept { return !(*this == other); }

    /// Compact serialisation.
    [[nodiscard]] std::string dump() const;

    /// Pretty serialisation with the given indent width.
    [[nodiscard]] std::string dump(int indent) const;

 private:
    std::variant<std::monostate,
                 bool,
                 std::int64_t,
                 double,
                 std::string,
                 ArrayStorage,
                 ObjectStorage>
        data_;
};

/// Public spellings of the container types. Declared here, after Value, for
/// the reason given at the top of this file.
using Array = Value::ArrayStorage;
using Member = Value::MemberStorage;
using Object = Value::ObjectStorage;

/// Limits applied while parsing. They exist because TestForge parses documents
/// that come from the network and from a language model, neither of which is
/// trusted to be well-behaved.
struct ParseLimits {
    std::size_t maxDepth = 64;                    ///< guards against stack exhaustion
    std::size_t maxLength = 16u * 1024u * 1024u;  ///< 16 MiB
};

/// Parses `text`, throwing ParseError on any problem.
Value parse(std::string_view text, const ParseLimits& limits = {});

/// Non-throwing variant. On failure returns nullopt and, if `error` is
/// non-null, stores a human readable description there.
std::optional<Value> tryParse(std::string_view text,
                              std::string* error = nullptr,
                              const ParseLimits& limits = {});

/// Escapes `text` as a JSON string literal, including the surrounding quotes.
std::string quote(std::string_view text);

// ---------------------------------------------------------------------------
// Safe readers
//
// `document.find("k")->asString()` is a null dereference waiting for the first
// document that does not have "k" — and every document these read comes from
// somewhere unreliable: a database column, an HTTP response, a language model.
// These helpers take a dotted path, never dereference a missing member, and
// return the fallback instead.
// ---------------------------------------------------------------------------

std::string stringAt(const Value& value,
                     std::string_view dottedPath,
                     std::string_view fallback = {});

std::int64_t intAt(const Value& value, std::string_view dottedPath, std::int64_t fallback = 0);

double doubleAt(const Value& value, std::string_view dottedPath, double fallback = 0.0);

bool boolAt(const Value& value, std::string_view dottedPath, bool fallback = false);

/// The array at `dottedPath`, or a reference to a shared empty array when the
/// path is missing or holds something else. Safe to iterate unconditionally.
const Array& arrayAt(const Value& value, std::string_view dottedPath);

}  // namespace testforge::json
