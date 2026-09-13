#include "testforge/ai/MockAiProvider.hpp"

#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <array>

namespace testforge::ai {
namespace {

TestSpec makeSpec(std::string name,
                  std::string method,
                  std::string endpoint,
                  int status,
                  std::string description) {
    TestSpec spec;
    spec.name = std::move(name);
    spec.method = std::move(method);
    spec.endpoint = std::move(endpoint);
    spec.expectedStatus = status;
    spec.description = std::move(description);
    spec.tags = {"generated", "mock"};
    return spec;
}

/// Extracts the first plausible resource noun from the requirement so the
/// generated endpoints look related to what was asked for.
std::string guessResource(std::string_view requirement) {
    static constexpr std::array<std::string_view, 6> kKnown = {
        "users", "items", "orders", "products", "accounts", "sessions"};
    for (const std::string_view candidate : kKnown) {
        if (strings::containsIgnoreCase(requirement, candidate)) {
            return std::string(candidate);
        }
        // Singular form: "a user registration API" -> "users".
        if (candidate.size() > 1 &&
            strings::containsIgnoreCase(requirement, candidate.substr(0, candidate.size() - 1))) {
            return std::string(candidate);
        }
    }
    return "items";
}

}  // namespace

GenerationResult MockAiProvider::generateTests(const GenerationRequest& request) {
    GenerationResult result;
    result.provider = "mock";
    result.model = "mock-deterministic-v1";

    if (!forcedError_.empty()) {
        result.error = forcedError_;
        return result;
    }

    const std::string resource = guessResource(request.requirement);
    const std::string collection = "/" + resource;

    TestSpecSuite suite;
    suite.suite = request.suiteName.empty() ? "generated" : request.suiteName;
    suite.requirement = request.requirement;
    suite.source = "mock";
    suite.model = result.model;

    // A baseline every API should satisfy.
    {
        TestSpec spec = makeSpec("list_" + resource,
                                 "GET",
                                 collection,
                                 200,
                                 "MOCK: the collection endpoint responds successfully");
        SpecAssertion latency;
        latency.kind = SpecAssertion::Kind::ResponseTimeUnder;
        latency.expected = json::Value(static_cast<std::int64_t>(2000));
        spec.assertions.push_back(latency);
        spec.maxResponseTimeMs = 2000;
        suite.tests.push_back(std::move(spec));
    }
    {
        TestSpec spec = makeSpec("missing_" + resource + "_returns_404",
                                 "GET",
                                 collection + "/999999",
                                 404,
                                 "MOCK: an unknown identifier produces a 404");
        suite.tests.push_back(std::move(spec));
    }

    // Rules keyed off the requirement text. Crude on purpose: the point is a
    // deterministic pipeline, not a clever one.
    const bool mentionsCreate = strings::containsIgnoreCase(request.requirement, "creat") ||
                                strings::containsIgnoreCase(request.requirement, "regist") ||
                                strings::containsIgnoreCase(request.requirement, "post") ||
                                strings::containsIgnoreCase(request.requirement, "sign up");
    if (mentionsCreate) {
        TestSpec spec = makeSpec("create_valid_" + resource,
                                 "POST",
                                 collection,
                                 201,
                                 "MOCK: a well-formed creation request succeeds");
        json::Value body = json::Value::object();
        body.set("name", "testforge-mock");
        body.set("email", "mock@example.com");
        body.set("password", "mock-password-1234");
        spec.body = body;
        SpecAssertion hasId;
        hasId.kind = SpecAssertion::Kind::JsonFieldExists;
        hasId.target = "id";
        spec.assertions.push_back(hasId);
        suite.tests.push_back(std::move(spec));

        TestSpec missingField = makeSpec("create_missing_required_field",
                                         "POST",
                                         collection,
                                         400,
                                         "MOCK: a request missing a required field is rejected");
        json::Value partial = json::Value::object();
        partial.set("name", "testforge-mock");
        missingField.body = partial;
        suite.tests.push_back(std::move(missingField));
    }

    const bool mentionsValidation = strings::containsIgnoreCase(request.requirement, "valid") ||
                                    strings::containsIgnoreCase(request.requirement, "requir") ||
                                    strings::containsIgnoreCase(request.requirement, "must ");
    if (mentionsValidation) {
        TestSpec spec = makeSpec("reject_malformed_payload",
                                 "POST",
                                 collection,
                                 400,
                                 "MOCK: a payload that violates the stated rules is rejected");
        json::Value body = json::Value::object();
        body.set("email", "not-an-email");
        body.set("password", "short");
        spec.body = body;
        suite.tests.push_back(std::move(spec));
    }

    const bool mentionsUnique = strings::containsIgnoreCase(request.requirement, "uniq");
    if (mentionsUnique) {
        TestSpec spec = makeSpec("duplicate_is_rejected",
                                 "POST",
                                 collection,
                                 409,
                                 "MOCK: creating a duplicate conflicts");
        json::Value body = json::Value::object();
        body.set("name", "testforge-mock-duplicate");
        body.set("email", "duplicate@example.com");
        body.set("password", "mock-password-1234");
        spec.body = body;
        suite.tests.push_back(std::move(spec));
    }

    const auto limit = static_cast<std::size_t>(std::max(1, request.maxTests));
    if (suite.tests.size() > limit) {
        suite.tests.resize(limit);
    }

    result.suite = std::move(suite);
    result.ok = true;
    result.elapsed = Milliseconds{1};
    result.promptTokens = static_cast<std::int64_t>(request.requirement.size() / 4);
    result.completionTokens = static_cast<std::int64_t>(result.suite.tests.size() * 40);
    return result;
}

AnalysisResult MockAiProvider::analyseFailure(const AnalysisRequest& request) {
    AnalysisResult result;
    result.provider = "mock";
    result.model = "mock-deterministic-v1";

    if (!forcedError_.empty()) {
        result.error = forcedError_;
        return result;
    }

    // Read the same structured evidence a real model would be given, and apply
    // the obvious deterministic reading of it.
    const json::Value& context = request.context;
    const json::Value* status = context.path("result.metadata.http_status");
    const json::Value* transport = context.path("result.metadata.transport_error");
    const std::string category = request.failureCategory;

    if (transport != nullptr && transport->isString()) {
        result.probableCause =
            "MOCK ANALYSIS: the request never reached the service — the transport layer reported "
            "'" +
            transport->asString() + "'.";
        result.suggestedCategory = "NETWORK_FAILURE";
        result.evidence.push_back("transport_error = " + transport->asString());
        result.suggestedInvestigation.emplace_back(
            "Confirm the system under test is running and listening on the configured base URL.");
        result.suggestedInvestigation.emplace_back(
            "Check for a port mismatch between the service and api.base_url.");
        result.confidence = 0.8;
    } else if (status != nullptr && status->intOr(0) >= 500) {
        result.probableCause =
            "MOCK ANALYSIS: the endpoint returned HTTP " + std::to_string(status->intOr(0)) +
            " while the request itself looked well-formed, which points at a server-side fault.";
        result.suggestedCategory = "APPLICATION_FAILURE";
        result.evidence.push_back("http_status = " + std::to_string(status->intOr(0)));
        if (const json::Value* excerpt = context.path("result.metadata.response_excerpt");
            excerpt != nullptr && excerpt->isString()) {
            result.evidence.push_back("response excerpt: " +
                                      strings::truncate(excerpt->asString(), 200));
        }
        result.suggestedInvestigation.emplace_back("Inspect the service's exception log.");
        result.suggestedInvestigation.emplace_back(
            "Reproduce the request by hand and check whether it is input-dependent.");
        result.confidence = 0.7;
    } else if (category == "TIMEOUT") {
        result.probableCause =
            "MOCK ANALYSIS: the test exceeded its deadline without producing a verdict.";
        result.suggestedCategory = "TIMEOUT";
        result.evidence.emplace_back("failure_category = TIMEOUT");
        if (const json::Value* load = context.path("diagnostics.system.cpu.load_average_1m");
            load != nullptr && load->isNumber()) {
            result.evidence.push_back("1-minute load average = " +
                                      std::to_string(load->doubleOr(0.0)));
        }
        result.suggestedInvestigation.emplace_back(
            "Check whether the machine was saturated when the test ran.");
        result.suggestedInvestigation.emplace_back(
            "Raise the timeout only after confirming the work genuinely takes that long.");
        result.confidence = 0.6;
    } else {
        result.probableCause =
            "MOCK ANALYSIS: an assertion did not hold. The expected and actual values in the "
            "failure detail are the place to start.";
        result.suggestedCategory = category.empty() ? "ASSERTION_FAILURE" : category;
        if (const json::Value* message = context.path("result.error_message");
            message != nullptr && message->isString()) {
            result.evidence.push_back(strings::truncate(message->asString(), 300));
        }
        result.suggestedInvestigation.emplace_back(
            "Decide whether the system changed behaviour or the expectation is stale.");
        result.confidence = 0.5;
    }

    result.ok = true;
    result.elapsed = Milliseconds{1};
    return result;
}

json::Value MockAiProvider::health() {
    json::Value out = json::Value::object();
    out.set("provider", "mock");
    out.set("enabled", true);
    out.set("reachable", true);
    out.set("model", "mock-deterministic-v1");
    out.set("is_mock", true);
    out.set("note",
            "Deterministic offline stand-in. No model is contacted and no output here reflects "
            "a real language model.");
    return out;
}

}  // namespace testforge::ai
