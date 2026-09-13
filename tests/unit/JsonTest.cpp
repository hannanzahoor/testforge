/// Tests for the bundled JSON implementation.
///
/// This library sits under everything — config, reports, the database, the AI
/// contract — and it parses input from the network and from a language model.
/// A parser bug here would be both silent and everywhere, so the coverage
/// includes the awkward cases (surrogate pairs, deep nesting, precision) as
/// well as the obvious ones.

#include "testforge/core/Json.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>

using testforge::json::Array;
using testforge::json::Object;
using testforge::json::ParseError;
using testforge::json::Value;

// ---------------------------------------------------------------------------
// Construction and access
// ---------------------------------------------------------------------------

TEST(Json, DefaultIsNull) {
    const Value value;
    EXPECT_TRUE(value.isNull());
    EXPECT_EQ(value.dump(), "null");
}

TEST(Json, ScalarsRoundTrip) {
    EXPECT_EQ(Value(true).dump(), "true");
    EXPECT_EQ(Value(false).dump(), "false");
    EXPECT_EQ(Value(42).dump(), "42");
    EXPECT_EQ(Value(-7).dump(), "-7");
    EXPECT_EQ(Value(std::string("hello")).dump(), "\"hello\"");
}

TEST(Json, IntegersAndDoublesAreDistinct) {
    const Value integer(42);
    const Value floating(42.0);

    EXPECT_TRUE(integer.isInteger());
    EXPECT_FALSE(integer.isDouble());
    EXPECT_TRUE(floating.isDouble());

    // A 64-bit id must survive without being degraded through a double, which
    // is the whole reason the two are separate alternatives.
    const std::int64_t large = 9007199254740993LL;  // 2^53 + 1
    EXPECT_EQ(Value(large).asInt(), large);
    EXPECT_EQ(Value(large).dump(), "9007199254740993");
}

TEST(Json, NumericComparisonIgnoresRepresentation) {
    EXPECT_TRUE(Value(1) == Value(1.0));
    EXPECT_FALSE(Value(1) == Value(2));
}

TEST(Json, TypedAccessorsThrowOnMismatch) {
    const Value text("hello");
    EXPECT_THROW((void)text.asInt(), testforge::json::TypeError);
    EXPECT_THROW((void)text.asArray(), testforge::json::TypeError);
    EXPECT_EQ(text.intOr(7), 7);
    EXPECT_EQ(text.stringOr("fallback"), "hello");
}

TEST(Json, ObjectPreservesInsertionOrder) {
    Value object = Value::object();
    object.set("zebra", 1);
    object.set("apple", 2);
    object.set("mango", 3);

    // Stable ordering is what makes report output byte-for-byte reproducible.
    EXPECT_EQ(object.dump(), R"({"zebra":1,"apple":2,"mango":3})");
}

TEST(Json, SetReplacesRatherThanDuplicates) {
    Value object = Value::object();
    object.set("key", 1);
    object.set("key", 2);
    EXPECT_EQ(object.size(), 1U);
    EXPECT_EQ(object.at("key").asInt(), 2);
}

TEST(Json, DottedPathLookup) {
    const Value document =
        testforge::json::parse(R"({"response":{"body":{"id":7,"tags":["a"]}},"top":1})");

    ASSERT_NE(document.path("response.body.id"), nullptr);
    EXPECT_EQ(document.path("response.body.id")->asInt(), 7);
    EXPECT_EQ(document.path("response.missing"), nullptr);
    EXPECT_EQ(document.path("nope.at.all"), nullptr);
    EXPECT_EQ(document.path(""), nullptr);
}

TEST(Json, ObjectEqualityIgnoresKeyOrder) {
    const Value a = testforge::json::parse(R"({"x":1,"y":2})");
    const Value b = testforge::json::parse(R"({"y":2,"x":1})");
    EXPECT_TRUE(a == b);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST(JsonParse, HandlesEveryScalarType) {
    const Value value =
        testforge::json::parse(R"({"n":null,"t":true,"f":false,"i":-12,"d":3.5,"e":1e3,"s":"x"})");

    EXPECT_TRUE(value.at("n").isNull());
    EXPECT_TRUE(value.at("t").asBool());
    EXPECT_FALSE(value.at("f").asBool());
    EXPECT_EQ(value.at("i").asInt(), -12);
    EXPECT_DOUBLE_EQ(value.at("d").asDouble(), 3.5);
    EXPECT_DOUBLE_EQ(value.at("e").asDouble(), 1000.0);
    EXPECT_EQ(value.at("s").asString(), "x");
}

TEST(JsonParse, NestedStructures) {
    const Value value = testforge::json::parse(R"({"a":[1,[2,[3,{"b":4}]]]})");
    EXPECT_EQ(value.at("a")[1][1][1].at("b").asInt(), 4);
}

TEST(JsonParse, EmptyContainers) {
    EXPECT_EQ(testforge::json::parse("[]").dump(), "[]");
    EXPECT_EQ(testforge::json::parse("{}").dump(), "{}");
    EXPECT_EQ(testforge::json::parse(R"({"a":[],"b":{}})").size(), 2U);
}

TEST(JsonParse, StringEscapes) {
    const Value value = testforge::json::parse(R"("line\nbreak\ttab\"quote\\slash\/solidusA")");
    EXPECT_EQ(value.asString(), "line\nbreak\ttab\"quote\\slash/solidusA");
}

TEST(JsonParse, SurrogatePairBecomesOneCodePoint) {
    // U+1F600, encoded as a UTF-16 surrogate pair, must come out as 4 UTF-8
    // bytes rather than two broken 3-byte sequences.
    const Value value = testforge::json::parse(R"("😀")");
    EXPECT_EQ(value.asString(), "\xF0\x9F\x98\x80");
}

TEST(JsonParse, LoneSurrogateBecomesReplacementCharacter) {
    // Invalid input must not produce invalid UTF-8 that poisons a consumer
    // downstream; U+FFFD is the standard substitution.
    const Value value = testforge::json::parse(R"("\ud83d")");
    EXPECT_EQ(value.asString(), "\xEF\xBF\xBD");
}

TEST(JsonParse, RejectsMalformedInput) {
    const char* cases[] = {
        "",
        "{",
        "[",
        "{\"a\"}",
        "{\"a\":}",
        "[1,]",
        "{,}",
        "tru",
        "01",
        "1.",
        "1e",
        "\"unterminated",
        "{\"a\":1}}",
        "[1 2]",
    };
    for (const char* text : cases) {
        EXPECT_THROW((void)testforge::json::parse(text), ParseError)
            << "should have rejected: " << text;
    }
}

TEST(JsonParse, RejectsUnescapedControlCharacters) {
    const std::string text = std::string("\"bad") + '\n' + "\"";
    EXPECT_THROW((void)testforge::json::parse(text), ParseError);
}

TEST(JsonParse, ErrorReportsLineAndColumn) {
    try {
        (void)testforge::json::parse("{\n  \"a\": ,\n}");
        FAIL() << "expected a ParseError";
    } catch (const ParseError& error) {
        EXPECT_EQ(error.line(), 2U);
        EXPECT_GT(error.column(), 1U);
        EXPECT_NE(std::string(error.what()).find("line 2"), std::string::npos);
    }
}

TEST(JsonParse, DepthLimitIsEnforced) {
    // Guards against stack exhaustion from a hostile document. The limit is
    // low here so the test stays fast.
    testforge::json::ParseLimits limits;
    limits.maxDepth = 5;

    std::string deep;
    for (int i = 0; i < 20; ++i) {
        deep += "[";
    }
    for (int i = 0; i < 20; ++i) {
        deep += "]";
    }
    EXPECT_THROW((void)testforge::json::parse(deep, limits), ParseError);
}

TEST(JsonParse, LengthLimitIsEnforced) {
    testforge::json::ParseLimits limits;
    limits.maxLength = 10;
    EXPECT_THROW((void)testforge::json::parse(R"({"key":"a much longer value"})", limits),
                 ParseError);
}

TEST(JsonParse, DuplicateKeysKeepTheLastValue) {
    const Value value = testforge::json::parse(R"({"a":1,"a":2})");
    EXPECT_EQ(value.size(), 1U);
    EXPECT_EQ(value.at("a").asInt(), 2);
}

TEST(JsonParse, TryParseReportsInsteadOfThrowing) {
    std::string error;
    EXPECT_FALSE(testforge::json::tryParse("{oops}", &error).has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(testforge::json::tryParse("{}", &error).has_value());
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

TEST(JsonDump, EscapesControlCharacters) {
    Value value = Value::object();
    value.set("text", std::string("a\nb\tc\"d\\e") + '\x01');
    // An ordinary literal rather than a raw one: GCC applies
    // universal-character-name conversion before raw-string processing, so
    // R"()" would become a real 0x01 byte instead of the six characters
    // the serialiser is expected to emit.
    EXPECT_EQ(value.dump(), "{\"text\":\"a\\nb\\tc\\\"d\\\\e\\u0001\"}");
}

TEST(JsonDump, PassesUtf8Through) {
    Value value = Value::object();
    value.set("emoji", "\xF0\x9F\x98\x80");
    const std::string dumped = value.dump();
    EXPECT_NE(dumped.find("\xF0\x9F\x98\x80"), std::string::npos);
    EXPECT_EQ(testforge::json::parse(dumped).at("emoji").asString(), "\xF0\x9F\x98\x80");
}

TEST(JsonDump, DoublesRoundTripExactly) {
    for (const double sample : {0.1, 1.0 / 3.0, 1e-300, 1e300, -2.5, 3.14159265358979}) {
        const Value value(sample);
        const Value again = testforge::json::parse(value.dump());
        EXPECT_DOUBLE_EQ(again.asDouble(), sample) << "failed for " << sample;
    }
}

TEST(JsonDump, DoubleKeepsItsType) {
    // 42.0 must not serialise as "42" and come back as an integer.
    EXPECT_TRUE(testforge::json::parse(Value(42.0).dump()).isDouble());
}

TEST(JsonDump, NonFiniteBecomesNull) {
    // JSON has no representation for these; null keeps the document parseable.
    EXPECT_EQ(Value(std::numeric_limits<double>::infinity()).dump(), "null");
    EXPECT_EQ(Value(std::numeric_limits<double>::quiet_NaN()).dump(), "null");
}

TEST(JsonDump, PrettyPrintingIsReparseable) {
    const Value value = testforge::json::parse(R"({"a":[1,2,{"b":"c"}],"d":{"e":null},"f":[]})");
    const std::string pretty = value.dump(2);

    EXPECT_NE(pretty.find('\n'), std::string::npos);
    EXPECT_TRUE(testforge::json::parse(pretty) == value);
}

TEST(JsonDump, RoundTripIsStable) {
    const std::string source =
        R"({"suite":"api","tests":[{"name":"a","status":200},{"name":"b","status":404}]})";
    const Value once = testforge::json::parse(source);
    const Value twice = testforge::json::parse(once.dump());
    EXPECT_EQ(once.dump(), twice.dump());
}

TEST(Json, QuoteHelper) {
    EXPECT_EQ(testforge::json::quote("a\"b"), R"("a\"b")");
}
