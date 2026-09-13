#include "testforge/ai/TestSpec.hpp"

#include "testforge/core/StringUtils.hpp"

#include <array>
#include <sstream>
#include <utility>

namespace testforge::ai {
namespace {

constexpr std::array<std::pair<SpecAssertion::Kind, std::string_view>, 9> kKindNames = {{
    {SpecAssertion::Kind::StatusCode, "status_code"},
    {SpecAssertion::Kind::StatusCodeIn, "status_code_in"},
    {SpecAssertion::Kind::ResponseTimeUnder, "response_time_under_ms"},
    {SpecAssertion::Kind::BodyContains, "body_contains"},
    {SpecAssertion::Kind::BodyNotContains, "body_not_contains"},
    {SpecAssertion::Kind::JsonFieldExists, "json_field_exists"},
    {SpecAssertion::Kind::JsonFieldEquals, "json_field_equals"},
    {SpecAssertion::Kind::HeaderExists, "header_exists"},
    {SpecAssertion::Kind::HeaderEquals, "header_equals"},
}};

std::string readString(const json::Value& object,
                       std::string_view key,
                       std::string_view fallback = {}) {
    const json::Value* found = object.find(key);
    return (found != nullptr && found->isString()) ? found->asString() : std::string(fallback);
}

}  // namespace

std::string_view SpecAssertion::kindName(Kind kind) {
    for (const auto& [candidate, name] : kKindNames) {
        if (candidate == kind) {
            return name;
        }
    }
    return "unknown";
}

std::optional<SpecAssertion::Kind> SpecAssertion::kindFromString(std::string_view text) {
    const std::string normalised = strings::toLower(strings::trim(text));
    for (const auto& [kind, name] : kKindNames) {
        if (normalised == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string SpecAssertion::describe() const {
    std::ostringstream os;
    switch (kind) {
        case Kind::StatusCode:
            os << "status == " << expected.dump();
            break;
        case Kind::StatusCodeIn:
            os << "status in " << expected.dump();
            break;
        case Kind::ResponseTimeUnder:
            os << "response time < " << expected.dump() << "ms";
            break;
        case Kind::BodyContains:
            os << "body contains " << expected.dump();
            break;
        case Kind::BodyNotContains:
            os << "body does not contain " << expected.dump();
            break;
        case Kind::JsonFieldExists:
            os << "json has field '" << target << "'";
            break;
        case Kind::JsonFieldEquals:
            os << "json." << target << " == " << expected.dump();
            break;
        case Kind::HeaderExists:
            os << "header '" << target << "' present";
            break;
        case Kind::HeaderEquals:
            os << "header '" << target << "' == " << expected.dump();
            break;
    }
    return os.str();
}

json::Value SpecAssertion::toJson() const {
    json::Value out = json::Value::object();
    out.set("kind", std::string(kindName(kind)));
    if (!target.empty()) {
        out.set("target", target);
    }
    out.set("expected", expected);
    return out;
}

std::optional<SpecAssertion> SpecAssertion::fromJson(const json::Value& value, std::string* error) {
    const auto fail = [error](const std::string& message) -> std::optional<SpecAssertion> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };

    if (!value.isObject()) {
        return fail("assertion must be an object");
    }
    const std::string kindText = readString(value, "kind");
    const std::optional<Kind> kind = kindFromString(kindText);
    if (!kind.has_value()) {
        // Naming the supported kinds turns a rejection into something the
        // caller (or the model, on a retry) can act on.
        std::ostringstream os;
        os << "unknown assertion kind '" << kindText << "'; supported kinds are ";
        for (std::size_t i = 0; i < kKindNames.size(); ++i) {
            if (i > 0) {
                os << ", ";
            }
            os << kKindNames[i].second;
        }
        return fail(os.str());
    }

    SpecAssertion assertion;
    assertion.kind = *kind;
    assertion.target = readString(value, "target");
    if (const json::Value* expected = value.find("expected"); expected != nullptr) {
        assertion.expected = *expected;
    }

    const bool needsTarget =
        assertion.kind == Kind::JsonFieldExists || assertion.kind == Kind::JsonFieldEquals ||
        assertion.kind == Kind::HeaderExists || assertion.kind == Kind::HeaderEquals;
    if (needsTarget && assertion.target.empty()) {
        return fail("assertion '" + std::string(kindName(assertion.kind)) +
                    "' requires a non-empty 'target'");
    }

    return assertion;
}

// ---------------------------------------------------------------------------
// TestSpec
// ---------------------------------------------------------------------------

json::Value TestSpec::toJson() const {
    json::Value out = json::Value::object();
    out.set("name", name);
    out.set("description", description);
    out.set("method", method);
    out.set("endpoint", endpoint);

    if (!headers.empty()) {
        json::Value headerJson = json::Value::object();
        for (const json::Member& header : headers) {
            headerJson.set(header.first, header.second);
        }
        out.set("headers", headerJson);
    }
    if (!body.isNull()) {
        out.set("body", body);
    }
    out.set("expected_status", expectedStatus);
    if (!acceptableStatuses.empty()) {
        json::Value statuses = json::Value::array();
        for (const int status : acceptableStatuses) {
            statuses.push(status);
        }
        out.set("acceptable_statuses", statuses);
    }
    if (maxResponseTimeMs > 0) {
        out.set("max_response_time_ms", maxResponseTimeMs);
    }
    if (!assertions.empty()) {
        json::Value list = json::Value::array();
        for (const SpecAssertion& assertion : assertions) {
            list.push(assertion.toJson());
        }
        out.set("assertions", list);
    }
    if (!tags.empty()) {
        json::Value list = json::Value::array();
        for (const std::string& tag : tags) {
            list.push(tag);
        }
        out.set("tags", list);
    }
    return out;
}

std::optional<TestSpec> TestSpec::fromJson(const json::Value& value, std::string* error) {
    const auto fail = [error](const std::string& message) -> std::optional<TestSpec> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };

    if (!value.isObject()) {
        return fail("test must be an object");
    }

    TestSpec spec;
    spec.name = strings::trim(readString(value, "name"));
    if (spec.name.empty()) {
        return fail("test is missing a 'name'");
    }
    spec.description = readString(value, "description");
    spec.method = strings::toUpper(strings::trim(readString(value, "method", "GET")));
    spec.endpoint = strings::trim(readString(value, "endpoint"));
    if (spec.endpoint.empty()) {
        // Accept "path" as a synonym: models produce it about as often.
        spec.endpoint = strings::trim(readString(value, "path"));
    }
    if (spec.endpoint.empty()) {
        return fail("test '" + spec.name + "' is missing an 'endpoint'");
    }

    if (const json::Value* headers = value.find("headers");
        headers != nullptr && headers->isObject()) {
        spec.headers = headers->asObject();
    }
    if (const json::Value* body = value.find("body"); body != nullptr) {
        spec.body = *body;
    }

    if (const json::Value* status = value.find("expected_status");
        status != nullptr && status->isNumber()) {
        spec.expectedStatus = static_cast<int>(status->asInt());
    } else if (const json::Value* alternative = value.find("expectedStatus");
               alternative != nullptr && alternative->isNumber()) {
        spec.expectedStatus = static_cast<int>(alternative->asInt());
    }

    if (const json::Value* statuses = value.find("acceptable_statuses");
        statuses != nullptr && statuses->isArray()) {
        for (const json::Value& item : statuses->asArray()) {
            if (item.isNumber()) {
                spec.acceptableStatuses.push_back(static_cast<int>(item.asInt()));
            }
        }
    }

    if (const json::Value* budget = value.find("max_response_time_ms");
        budget != nullptr && budget->isNumber()) {
        spec.maxResponseTimeMs = budget->asInt();
    }

    if (const json::Value* assertions = value.find("assertions");
        assertions != nullptr && assertions->isArray()) {
        for (const json::Value& item : assertions->asArray()) {
            std::string assertionError;
            std::optional<SpecAssertion> assertion = SpecAssertion::fromJson(item, &assertionError);
            if (!assertion.has_value()) {
                return fail("test '" + spec.name + "': " + assertionError);
            }
            spec.assertions.push_back(std::move(*assertion));
        }
    }

    if (const json::Value* tags = value.find("tags"); tags != nullptr && tags->isArray()) {
        for (const json::Value& item : tags->asArray()) {
            if (item.isString()) {
                spec.tags.push_back(item.asString());
            }
        }
    }

    return spec;
}

// ---------------------------------------------------------------------------
// TestSpecSuite
// ---------------------------------------------------------------------------

json::Value TestSpecSuite::toJson() const {
    json::Value out = json::Value::object();
    out.set("suite", suite);
    out.set("requirement", requirement);
    out.set("source", source);
    if (!model.empty()) {
        out.set("model", model);
    }
    json::Value list = json::Value::array();
    for (const TestSpec& spec : tests) {
        list.push(spec.toJson());
    }
    out.set("tests", list);
    out.set("test_count", static_cast<std::int64_t>(tests.size()));
    return out;
}

std::optional<TestSpecSuite> TestSpecSuite::fromJson(const json::Value& value, std::string* error) {
    const auto fail = [error](const std::string& message) -> std::optional<TestSpecSuite> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };

    if (!value.isObject()) {
        return fail("the specification document must be a JSON object");
    }

    TestSpecSuite suite;
    suite.suite = strings::trim(readString(value, "suite"));
    if (suite.suite.empty()) {
        return fail("the specification is missing a 'suite' name");
    }
    suite.requirement = readString(value, "requirement");
    suite.source = readString(value, "source");
    suite.model = readString(value, "model");

    const json::Value* tests = value.find("tests");
    if (tests == nullptr || !tests->isArray()) {
        return fail("the specification is missing a 'tests' array");
    }
    if (tests->asArray().empty()) {
        return fail("the specification contains no tests");
    }

    for (const json::Value& item : tests->asArray()) {
        std::string testError;
        std::optional<TestSpec> spec = TestSpec::fromJson(item, &testError);
        if (!spec.has_value()) {
            return fail(testError);
        }
        suite.tests.push_back(std::move(*spec));
    }

    return suite;
}

std::optional<TestSpecSuite> TestSpecSuite::parse(std::string_view text, std::string* error) {
    std::string parseError;
    // Bound the document: this is the entry point for model output and for
    // files supplied on the command line, neither of which is trusted to be a
    // reasonable size.
    json::ParseLimits limits;
    limits.maxLength = 2u * 1024u * 1024u;
    limits.maxDepth = 24;

    const std::optional<json::Value> document = json::tryParse(text, &parseError, limits);
    if (!document.has_value()) {
        if (error != nullptr) {
            *error = "the specification is not valid JSON: " + parseError;
        }
        return std::nullopt;
    }
    return fromJson(*document, error);
}

}  // namespace testforge::ai
