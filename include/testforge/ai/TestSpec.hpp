#pragma once

#include "testforge/core/Json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace testforge::ai {

/// One check inside a generated test.
///
/// A closed set of assertion kinds is the core of the safety model: a
/// generated test can only ask for checks that already exist in this enum, so
/// there is no path from model output to arbitrary behaviour. Adding a new
/// kind is a deliberate act by a human editing this file.
struct SpecAssertion {
    enum class Kind : std::uint8_t {
        StatusCode,
        StatusCodeIn,
        ResponseTimeUnder,
        BodyContains,
        BodyNotContains,
        JsonFieldExists,
        JsonFieldEquals,
        HeaderExists,
        HeaderEquals,
    };

    Kind kind = Kind::StatusCode;
    std::string target;    ///< field path, header name, or empty
    json::Value expected;  ///< the value to compare against

    [[nodiscard]] std::string describe() const;

    [[nodiscard]] json::Value toJson() const;

    static std::optional<SpecAssertion> fromJson(const json::Value& value, std::string* error);

    static std::string_view kindName(Kind kind);

    static std::optional<Kind> kindFromString(std::string_view text);
};

/// A declarative HTTP test.
///
/// This type is the entire vocabulary available to generated tests. It cannot
/// express "run this command" or "call this function", because it has no field
/// for either — which is what makes executing model output safe.
struct TestSpec {
    std::string name;
    std::string description;
    std::string method = "GET";

    /// A path relative to the configured base URL ("/users/1"). Absolute URLs
    /// are rejected by the validator: a generated test may not choose its own
    /// destination host.
    std::string endpoint;

    std::vector<json::Member> headers;
    json::Value body;  ///< null means no body

    int expectedStatus = 200;
    std::vector<int> acceptableStatuses;  ///< when set, overrides expectedStatus

    /// 0 means "no latency assertion".
    std::int64_t maxResponseTimeMs = 0;

    std::vector<SpecAssertion> assertions;
    std::vector<std::string> tags;

    [[nodiscard]] json::Value toJson() const;

    static std::optional<TestSpec> fromJson(const json::Value& value, std::string* error);
};

/// A named group of generated tests, plus provenance.
struct TestSpecSuite {
    std::string suite;
    std::string requirement;  ///< the natural-language input, kept for the record
    std::string source;       ///< "ai:gpt-4o-mini", "file:specs/users.json", "mock"
    std::string model;
    std::vector<TestSpec> tests;

    [[nodiscard]] json::Value toJson() const;

    static std::optional<TestSpecSuite> fromJson(const json::Value& value, std::string* error);

    /// Parses a JSON document. Returns nullopt and sets `error` on any problem.
    static std::optional<TestSpecSuite> parse(std::string_view json, std::string* error);
};

}  // namespace testforge::ai
