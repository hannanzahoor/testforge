#include "testforge/testing/SpecTestCase.hpp"

#include "testforge/ai/SpecValidator.hpp"
#include "testforge/core/Exceptions.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/net/Url.hpp"
#include "testforge/testing/ApiTestCase.hpp"
#include "testforge/testing/Assertions.hpp"
#include "testforge/testing/TestRegistry.hpp"

#include <utility>

namespace testforge {
namespace {

using ai::SpecAssertion;

void evaluate(const SpecAssertion& assertion,
              const net::HttpResponse& response,
              const SourceLocation& where) {
    switch (assertion.kind) {
        case SpecAssertion::Kind::StatusCode:
            assertions::statusCode(response.statusCode,
                                   static_cast<int>(assertion.expected.intOr(200)),
                                   response.describeTarget(),
                                   where);
            break;

        case SpecAssertion::Kind::StatusCodeIn: {
            std::vector<int> allowed;
            if (assertion.expected.isArray()) {
                for (const json::Value& item : assertion.expected.asArray()) {
                    if (item.isNumber()) {
                        allowed.push_back(static_cast<int>(item.asInt()));
                    }
                }
            }
            assertions::statusCodeIn(
                response.statusCode, allowed, response.describeTarget(), where);
            break;
        }

        case SpecAssertion::Kind::ResponseTimeUnder:
            assertions::responseTime(response.elapsed,
                                     Milliseconds{assertion.expected.intOr(0)},
                                     response.describeTarget(),
                                     where);
            break;

        case SpecAssertion::Kind::BodyContains:
            assertions::containsSubstring(response.body,
                                          assertion.expected.stringOr(""),
                                          "response body contains expected text",
                                          where);
            break;

        case SpecAssertion::Kind::BodyNotContains:
            assertions::doesNotContainSubstring(response.body,
                                                assertion.expected.stringOr(""),
                                                "response body excludes text",
                                                where);
            break;

        case SpecAssertion::Kind::JsonFieldExists: {
            const json::Value document = response.jsonOrThrow();
            assertions::jsonHasField(document, assertion.target, where);
            break;
        }

        case SpecAssertion::Kind::JsonFieldEquals: {
            const json::Value document = response.jsonOrThrow();
            assertions::jsonFieldEquals(document, assertion.target, assertion.expected, where);
            break;
        }

        case SpecAssertion::Kind::HeaderExists: {
            const bool present = response.headers.contains(assertion.target);
            AssertionEngine::report(present,
                                    "header '" + assertion.target + "' present",
                                    "header present",
                                    present ? "present" : "absent",
                                    "missing response header for " + response.describeTarget(),
                                    where);
            break;
        }

        case SpecAssertion::Kind::HeaderEquals: {
            const std::string actual = response.headers.getOr(assertion.target, "");
            const std::string expected = assertion.expected.stringOr("");
            AssertionEngine::report(actual == expected,
                                    "header '" + assertion.target + "' == expected",
                                    expected,
                                    actual.empty() ? "(absent)" : actual,
                                    "unexpected response header for " + response.describeTarget(),
                                    where);
            break;
        }
    }
}

}  // namespace

SpecTestCase::SpecTestCase(TestMetadata metadata, ai::TestSpec spec)
    : metadata_(std::move(metadata)), spec_(std::move(spec)) {}

void SpecTestCase::execute(TestContext& context) {
    ApiClient api(context);

    // Provenance goes on the result: someone reading a failure six weeks later
    // needs to know this test was machine-generated.
    context.addMetadata("spec_generated", true);
    context.addMetadata("spec_method", spec_.method);
    context.addMetadata("spec_endpoint", spec_.endpoint);
    if (!spec_.description.empty()) {
        context.addNote(spec_.description);
    }

    net::HttpRequest request;
    request.method = spec_.method;
    request.url = api.resolve(spec_.endpoint);
    for (const json::Member& header : spec_.headers) {
        if (header.second.isString()) {
            request.headers.set(header.first, header.second.asString());
        }
    }
    if (!spec_.body.isNull()) {
        request.body = spec_.body.dump();
        if (!request.headers.contains("Content-Type")) {
            request.headers.set("Content-Type", "application/json");
        }
    }

    context.log().info("executing generated test",
                       {{"method", spec_.method}, {"endpoint", spec_.endpoint}});

    const net::HttpResponse response = api.send(std::move(request));

    if (response.transportError) {
        // The service being unreachable is not evidence about the endpoint
        // under test, so report it as what it is.
        throw NetworkError("could not reach " + response.requestUrl + ": " + response.errorMessage);
    }

    // Collect every verdict rather than stopping at the first: a generated
    // test usually asserts several independent facts, and seeing all of them
    // is what makes the generated suite worth reading.
    SoftAssertionScope soft;
    const SourceLocation where = TESTFORGE_CURRENT_LOCATION();

    if (!spec_.acceptableStatuses.empty()) {
        assertions::statusCodeIn(
            response.statusCode, spec_.acceptableStatuses, response.describeTarget(), where);
    } else {
        assertions::statusCode(
            response.statusCode, spec_.expectedStatus, response.describeTarget(), where);
    }

    if (spec_.maxResponseTimeMs > 0) {
        assertions::responseTime(response.elapsed,
                                 Milliseconds{spec_.maxResponseTimeMs},
                                 response.describeTarget(),
                                 where);
    }

    for (const SpecAssertion& assertion : spec_.assertions) {
        evaluate(assertion, response, where);
    }

    soft.verify();
}

std::size_t registerSpecSuite(TestRegistry& registry,
                              const ai::TestSpecSuite& suite,
                              const Config& config) {
    // Re-validate rather than trust the caller. This function is reachable
    // from the REST API and from the CLI, and a specification that reaches
    // execution unvalidated is exactly the failure this design exists to
    // prevent.
    ai::TestSpecSuite copy = suite;
    ai::SpecValidator validator(
        config.ai, net::UrlPolicy::restrictedTo(config.api.baseUrl), config.api.baseUrl);
    const ai::ValidationReport report = validator.validate(copy);
    if (!report.valid) {
        throw SecurityError("refusing to register the generated suite: " + report.summary());
    }
    if (report.rejected > 0) {
        Logger("spec").warn(
            "some generated tests were rejected during registration",
            {{"suite", copy.suite}, {"rejected", report.rejected}, {"accepted", report.accepted}});
    }

    registry.removeSuite(copy.suite);

    std::size_t registered = 0;
    for (const ai::TestSpec& spec : copy.tests) {
        TestMetadata metadata;
        metadata.suite = copy.suite;
        metadata.name = spec.name;
        metadata.description = spec.description;
        metadata.tags = spec.tags;
        if (std::find(metadata.tags.begin(), metadata.tags.end(), "generated") ==
            metadata.tags.end()) {
            metadata.tags.emplace_back("generated");
        }
        if (std::find(metadata.tags.begin(), metadata.tags.end(), copy.suite) ==
            metadata.tags.end()) {
            metadata.tags.push_back(copy.suite);
        }
        metadata.owner = copy.source;
        metadata.sourceFile = copy.source;

        registry.registerOrReplace(metadata, [metadata, spec]() -> TestCasePtr {
            return std::make_unique<SpecTestCase>(metadata, spec);
        });
        ++registered;
    }

    return registered;
}

}  // namespace testforge
