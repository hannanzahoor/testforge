/// Tests for the AI layer.
///
/// The security boundary is SpecValidator, so that is where the weight of this
/// file sits: it is the code that decides whether text produced by a language
/// model is allowed to become an HTTP request. Everything a hostile
/// specification could try is checked here.

#include "testforge/ai/AiProvider.hpp"
#include "testforge/ai/FailureContext.hpp"
#include "testforge/ai/MockAiProvider.hpp"
#include "testforge/ai/SpecValidator.hpp"
#include "testforge/ai/TestSpec.hpp"
#include "testforge/core/Config.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace testforge;
using namespace testforge::ai;

// ---------------------------------------------------------------------------
// Specification parsing
// ---------------------------------------------------------------------------

TEST(TestSpec, ParsesAWellFormedDocument) {
    std::string error;
    const auto suite = TestSpecSuite::parse(R"({
        "suite": "generated",
        "requirement": "users must be unique",
        "tests": [
            {"name": "create_user", "method": "POST", "endpoint": "/users",
             "body": {"username": "x"}, "expected_status": 201,
             "max_response_time_ms": 2000,
             "assertions": [{"kind": "json_field_exists", "target": "id"}],
             "tags": ["generated"]}
        ]
    })",
                                            &error);

    ASSERT_TRUE(suite.has_value()) << error;
    EXPECT_EQ(suite->suite, "generated");
    ASSERT_EQ(suite->tests.size(), 1U);
    EXPECT_EQ(suite->tests[0].method, "POST");
    EXPECT_EQ(suite->tests[0].expectedStatus, 201);
    EXPECT_EQ(suite->tests[0].maxResponseTimeMs, 2000);
    ASSERT_EQ(suite->tests[0].assertions.size(), 1U);
    EXPECT_EQ(suite->tests[0].assertions[0].kind, SpecAssertion::Kind::JsonFieldExists);
}

TEST(TestSpec, AcceptsPathAsASynonymForEndpoint) {
    std::string error;
    const auto suite = TestSpecSuite::parse(
        R"({"suite":"s","tests":[{"name":"t","method":"GET","path":"/x"}]})", &error);
    ASSERT_TRUE(suite.has_value()) << error;
    EXPECT_EQ(suite->tests[0].endpoint, "/x");
}

TEST(TestSpec, RejectsStructurallyBrokenDocuments) {
    const char* cases[] = {
        "not json",
        "[]",
        R"({"tests": []})",                                 // no suite
        R"({"suite": "s"})",                                // no tests
        R"({"suite": "s", "tests": []})",                   // empty
        R"({"suite": "s", "tests": [{"method": "GET"}]})",  // no name
        R"({"suite": "s", "tests": [{"name": "t"}]})",      // no endpoint
    };
    for (const char* text : cases) {
        std::string error;
        EXPECT_FALSE(TestSpecSuite::parse(text, &error).has_value()) << text;
        EXPECT_FALSE(error.empty()) << text;
    }
}

TEST(TestSpec, RejectsAnUnknownAssertionKind) {
    std::string error;
    const auto suite = TestSpecSuite::parse(
        R"({"suite":"s","tests":[{"name":"t","method":"GET","endpoint":"/x",
             "assertions":[{"kind":"execute_shell_command","target":"rm -rf /"}]}]})",
        &error);
    ASSERT_FALSE(suite.has_value());
    // The message lists what is supported, so a retry can be informed.
    EXPECT_NE(error.find("execute_shell_command"), std::string::npos);
    EXPECT_NE(error.find("status_code"), std::string::npos);
}

TEST(TestSpec, SerialisationRoundTrips) {
    std::string error;
    const auto original = TestSpecSuite::parse(
        R"({"suite":"s","tests":[{"name":"t","method":"PUT","endpoint":"/x/1",
             "expected_status":200,"headers":{"Accept":"application/json"},
             "body":{"a":1},"assertions":[{"kind":"status_code","expected":200}]}]})",
        &error);
    ASSERT_TRUE(original.has_value()) << error;

    const auto again = TestSpecSuite::fromJson(original->toJson(), &error);
    ASSERT_TRUE(again.has_value()) << error;
    EXPECT_EQ(again->tests.size(), original->tests.size());
    EXPECT_EQ(again->tests[0].endpoint, "/x/1");
    EXPECT_EQ(again->tests[0].headers.size(), 1U);
}

TEST(TestSpec, ParsingIsBoundedInSize) {
    // The entry point for model output must not accept an unbounded document.
    std::string huge = R"({"suite":"s","requirement":")";
    huge += std::string(3 * 1024 * 1024, 'x');
    huge += R"(","tests":[{"name":"t","method":"GET","endpoint":"/x"}]})";

    std::string error;
    EXPECT_FALSE(TestSpecSuite::parse(huge, &error).has_value());
}

// ---------------------------------------------------------------------------
// SpecValidator — the security boundary
// ---------------------------------------------------------------------------

namespace {

SpecValidator makeValidator(const std::string& baseUrl = "http://127.0.0.1:8000") {
    AiConfig config;
    config.maxGeneratedTests = 25;
    return SpecValidator(config, net::UrlPolicy::restrictedTo(baseUrl), baseUrl);
}

TestSpec goodSpec(std::string name = "ok") {
    TestSpec spec;
    spec.name = std::move(name);
    spec.method = "GET";
    spec.endpoint = "/users";
    spec.expectedStatus = 200;
    return spec;
}

TestSpecSuite suiteOf(std::vector<TestSpec> tests) {
    TestSpecSuite suite;
    suite.suite = "generated";
    suite.tests = std::move(tests);
    return suite;
}

}  // namespace

TEST(SpecValidator, AcceptsAReasonableSuite) {
    TestSpecSuite suite = suiteOf({goodSpec("a"), goodSpec("b")});
    const ValidationReport report = makeValidator().validate(suite);

    EXPECT_TRUE(report.valid);
    EXPECT_EQ(report.accepted, 2);
    EXPECT_EQ(report.rejected, 0);
    EXPECT_EQ(suite.tests.size(), 2U);
}

TEST(SpecValidator, RejectsAbsoluteUrls) {
    // The core rule: a generated test may not choose its own destination.
    for (const char* endpoint : {"http://evil.test/steal",
                                 "https://evil.test/steal",
                                 "//evil.test/steal",
                                 "/redirect?to=http://evil.test"}) {
        TestSpec spec = goodSpec();
        spec.endpoint = endpoint;
        TestSpecSuite suite = suiteOf({spec});

        const ValidationReport report = makeValidator().validate(suite);
        EXPECT_FALSE(report.valid) << endpoint;
        EXPECT_TRUE(suite.tests.empty()) << endpoint;
    }
}

TEST(SpecValidator, RejectsPathTraversal) {
    TestSpec spec = goodSpec();
    spec.endpoint = "/api/../../etc/passwd";
    TestSpecSuite suite = suiteOf({spec});

    EXPECT_FALSE(makeValidator().validate(suite).valid);
}

TEST(SpecValidator, RejectsRequestSplitting) {
    // A CR or LF in an endpoint is HTTP request splitting.
    TestSpec spec = goodSpec();
    spec.endpoint = "/users\r\nX-Injected: yes";
    TestSpecSuite suite = suiteOf({spec});

    EXPECT_FALSE(makeValidator().validate(suite).valid);
}

TEST(SpecValidator, RejectsRelativeEndpointsWithoutALeadingSlash) {
    TestSpec spec = goodSpec();
    spec.endpoint = "users";
    TestSpecSuite suite = suiteOf({spec});
    EXPECT_FALSE(makeValidator().validate(suite).valid);
}

TEST(SpecValidator, RejectsDisallowedMethods) {
    for (const char* method : {"TRACE", "CONNECT", "BREW", ""}) {
        TestSpec spec = goodSpec();
        spec.method = method;
        TestSpecSuite suite = suiteOf({spec});
        EXPECT_FALSE(makeValidator().validate(suite).valid) << method;
    }
}

TEST(SpecValidator, RejectsCredentialAndRoutingHeaders) {
    // Credentials come from configuration, never from a model.
    for (const char* header : {"Authorization",
                               "authorization",
                               "Cookie",
                               "Host",
                               "X-Forwarded-For",
                               "Content-Length"}) {
        TestSpec spec = goodSpec();
        spec.headers.emplace_back(header, json::Value("value"));
        TestSpecSuite suite = suiteOf({spec});

        const ValidationReport report = makeValidator().validate(suite);
        EXPECT_FALSE(report.valid) << header;
    }
}

TEST(SpecValidator, AllowsOrdinaryHeaders) {
    TestSpec spec = goodSpec();
    spec.headers.emplace_back("Accept", json::Value("application/json"));
    spec.headers.emplace_back("X-Request-Id", json::Value("abc"));
    TestSpecSuite suite = suiteOf({spec});

    EXPECT_TRUE(makeValidator().validate(suite).valid);
}

TEST(SpecValidator, RejectsImpossibleStatusCodes) {
    for (const int status : {0, 99, 600, -1}) {
        TestSpec spec = goodSpec();
        spec.expectedStatus = status;
        TestSpecSuite suite = suiteOf({spec});
        EXPECT_FALSE(makeValidator().validate(suite).valid) << status;
    }
}

TEST(SpecValidator, RejectsUnsafeNames) {
    // A name becomes part of a qualified identifier, a filename and an HTML
    // fragment, so the character set is deliberately narrow.
    for (const char* name :
         {"", "../escape", "name with spaces", "name/slash", "name;semicolon", "<script>"}) {
        TestSpec spec = goodSpec(name);
        TestSpecSuite suite = suiteOf({spec});
        EXPECT_FALSE(makeValidator().validate(suite).valid) << name;
    }
}

TEST(SpecValidator, RejectsDuplicateNames) {
    // Two tests with one name means the second silently replaces the first.
    TestSpecSuite suite = suiteOf({goodSpec("same"), goodSpec("same")});
    const ValidationReport report = makeValidator().validate(suite);

    EXPECT_EQ(report.accepted, 1);
    EXPECT_EQ(report.rejected, 1);
    EXPECT_EQ(suite.tests.size(), 1U);
}

TEST(SpecValidator, EnforcesTheSuiteSizeCap) {
    AiConfig config;
    config.maxGeneratedTests = 3;
    SpecValidator validator(
        config, net::UrlPolicy::restrictedTo("http://127.0.0.1:8000"), "http://127.0.0.1:8000");

    std::vector<TestSpec> tests;
    for (int i = 0; i < 10; ++i) {
        tests.push_back(goodSpec("t" + std::to_string(i)));
    }
    TestSpecSuite suite = suiteOf(std::move(tests));

    const ValidationReport report = validator.validate(suite);
    EXPECT_EQ(suite.tests.size(), 3U);
    EXPECT_TRUE(report.valid);
}

TEST(SpecValidator, RejectsAnOversizedBody) {
    TestSpec spec = goodSpec();
    spec.method = "POST";
    spec.body = json::Value(std::string(100 * 1024, 'x'));
    TestSpecSuite suite = suiteOf({spec});

    EXPECT_FALSE(makeValidator().validate(suite).valid);
}

TEST(SpecValidator, BodyOnAGetIsAWarningNotARejection) {
    TestSpec spec = goodSpec();
    spec.method = "GET";
    spec.body = json::Value::object();
    spec.body.set("a", 1);
    TestSpecSuite suite = suiteOf({spec});

    const ValidationReport report = makeValidator().validate(suite);
    EXPECT_TRUE(report.valid) << "odd, but not dangerous";
    EXPECT_FALSE(report.issues.empty());
    EXPECT_FALSE(report.issues.front().fatal);
}

TEST(SpecValidator, OneBadTestDoesNotCondemnTheSuite) {
    TestSpec bad = goodSpec("bad");
    bad.endpoint = "http://evil.test/";
    TestSpecSuite suite = suiteOf({goodSpec("good"), bad, goodSpec("also_good")});

    const ValidationReport report = makeValidator().validate(suite);
    EXPECT_TRUE(report.valid);
    EXPECT_EQ(report.accepted, 2);
    EXPECT_EQ(report.rejected, 1);
    EXPECT_EQ(suite.tests.size(), 2U);
}

TEST(SpecValidator, AnUnsafeSuiteNameDiscardsEverything) {
    TestSpecSuite suite = suiteOf({goodSpec()});
    suite.suite = "../escape";

    const ValidationReport report = makeValidator().validate(suite);
    EXPECT_FALSE(report.valid);
    EXPECT_TRUE(suite.tests.empty());
}

TEST(SpecValidator, ReportSerialisesForTheApi) {
    TestSpecSuite suite = suiteOf({goodSpec("good"), goodSpec("")});
    const ValidationReport report = makeValidator().validate(suite);

    const json::Value document = report.toJson();
    EXPECT_TRUE(document.contains("valid"));
    EXPECT_TRUE(document.contains("accepted"));
    EXPECT_TRUE(document.contains("issues"));
    EXPECT_FALSE(report.summary().empty());
}

// ---------------------------------------------------------------------------
// Providers
// ---------------------------------------------------------------------------

TEST(DisabledProvider, ReportsWhyRatherThanFailing) {
    const AiProviderPtr provider = makeDisabledProvider("no key configured");
    EXPECT_FALSE(provider->isEnabled());

    const GenerationResult generation = provider->generateTests({});
    EXPECT_FALSE(generation.ok);
    EXPECT_EQ(generation.error, "no key configured");

    const AnalysisResult analysis = provider->analyseFailure({});
    EXPECT_FALSE(analysis.ok);

    const json::Value health = provider->health();
    EXPECT_FALSE(health.at("enabled").asBool());
    EXPECT_TRUE(health.contains("hint")) << "a disabled provider should say how to enable it";
}

TEST(CreateProvider, DefaultsToDisabled) {
    AiConfig config;
    config.enabled = false;
    EXPECT_FALSE(createProvider(config)->isEnabled());
}

TEST(MockProvider, IsDeterministic) {
    MockAiProvider provider;
    GenerationRequest request;
    request.requirement = "Create a user registration API. Usernames must be unique.";
    request.suiteName = "gen";
    request.maxTests = 10;

    const GenerationResult first = provider.generateTests(request);
    const GenerationResult second = provider.generateTests(request);

    ASSERT_TRUE(first.ok);
    EXPECT_EQ(first.suite.toJson().dump(), second.suite.toJson().dump());
}

TEST(MockProvider, RespondsToTheRequirementText) {
    MockAiProvider provider;
    GenerationRequest request;
    request.requirement = "Users must be created with a unique username.";
    request.maxTests = 10;

    const GenerationResult result = provider.generateTests(request);
    ASSERT_TRUE(result.ok);

    bool sawCreate = false;
    bool sawConflict = false;
    for (const TestSpec& spec : result.suite.tests) {
        sawCreate = sawCreate || spec.method == "POST";
        sawConflict = sawConflict || spec.expectedStatus == 409;
    }
    EXPECT_TRUE(sawCreate);
    EXPECT_TRUE(sawConflict) << "the word 'unique' should produce a conflict case";
}

TEST(MockProvider, OutputPassesValidation) {
    // The whole point of the mock is to exercise the real pipeline offline, so
    // what it produces must be acceptable to the real validator.
    MockAiProvider provider;
    GenerationRequest request;
    request.requirement = "Create and validate users; email is required.";
    request.maxTests = 10;

    GenerationResult result = provider.generateTests(request);
    ASSERT_TRUE(result.ok);

    const ValidationReport report = makeValidator().validate(result.suite);
    EXPECT_TRUE(report.valid);
    EXPECT_EQ(report.rejected, 0) << report.summary();
}

TEST(MockProvider, LabelsItselfAsMock) {
    MockAiProvider provider;
    const json::Value health = provider.health();
    EXPECT_TRUE(health.at("is_mock").asBool());

    const GenerationResult result = provider.generateTests({});
    EXPECT_EQ(result.provider, "mock");
    for (const TestSpec& spec : result.suite.tests) {
        EXPECT_NE(spec.description.find("MOCK"), std::string::npos);
    }
}

TEST(MockProvider, AnalysisReadsTheEvidence) {
    MockAiProvider provider;

    AnalysisRequest request;
    request.testName = "api.create";
    request.failureCategory = "ASSERTION_FAILURE";
    request.context = json::parse(R"({"result":{"metadata":{"http_status":500}}})");

    const AnalysisResult result = provider.analyseFailure(request);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.suggestedCategory, "APPLICATION_FAILURE");
    EXPECT_FALSE(result.evidence.empty());
    EXPECT_FALSE(result.suggestedInvestigation.empty());
}

TEST(MockProvider, FailureModeCanBeForced) {
    MockAiProvider provider;
    provider.setFailureMode("simulated outage");
    EXPECT_FALSE(provider.generateTests({}).ok);
    EXPECT_FALSE(provider.analyseFailure({}).ok);
}

TEST(AnalysisResult, AlwaysCarriesTheAdvisoryDisclaimer) {
    AnalysisResult result;
    result.ok = true;
    result.probableCause = "the service returned 500";
    result.confidence = 0.7;

    const json::Value document = result.toJson();
    // No consumer may render this as a verdict.
    EXPECT_TRUE(document.at("advisory_only").asBool());
    EXPECT_FALSE(document.at("disclaimer").asString().empty());
    EXPECT_NE(result.render().find("advisory"), std::string::npos);
}

// ---------------------------------------------------------------------------
// FailureContext
// ---------------------------------------------------------------------------

TEST(FailureContext, CarriesTheEvidenceAndLabelsItUntrusted) {
    TestResult result = TestResult::make("api", "broken", TestStatus::Failed);
    result.failureCategory = FailureCategory::ApplicationFailure;
    result.errorMessage = "expected 200, got 500";
    result.duration = Milliseconds{120};
    result.logs = {"line one", "line two"};
    result.metadata.set("http_status", 500);
    result.metadata.set("http_url", "http://host/users");

    const json::Value context = FailureContext{}.build(result);

    EXPECT_EQ(context.at("trust_level").asString(), "untrusted_data");
    EXPECT_EQ(context.path("test.name")->asString(), "api.broken");
    EXPECT_EQ(context.path("result.failure_category")->asString(), "APPLICATION_FAILURE");
    EXPECT_EQ(context.path("result.metadata.http_status")->asInt(), 500);
    EXPECT_EQ(context.path("result.log_tail")->size(), 2U);
    // The category's meaning travels with it, so the model does not have to
    // guess what the label means.
    EXPECT_FALSE(context.path("result.category_meaning")->asString().empty());
}

TEST(FailureContext, MetadataUsesAnAllowList) {
    TestResult result = TestResult::make("api", "t", TestStatus::Failed);
    result.metadata.set("http_status", 500);
    result.metadata.set("internal_session_token",
                        // NOT-A-SECRET: fixture string, never a live credential
                        "sk-abcdef1234567890");
    result.metadata.set("some_future_field", "should not leak by default");

    const json::Value context = FailureContext{}.build(result);
    const json::Value* metadata = context.path("result.metadata");
    ASSERT_NE(metadata, nullptr);

    EXPECT_TRUE(metadata->contains("http_status"));
    // Anything not on the list is omitted, so a new field cannot leak.
    EXPECT_FALSE(metadata->contains("internal_session_token"));
    EXPECT_FALSE(metadata->contains("some_future_field"));
}

TEST(FailureContext, RedactsWhatItDoesInclude) {
    TestResult result = TestResult::make("api", "t", TestStatus::Failed);
    result.errorMessage =
        // NOT-A-SECRET: fixture string, never a live credential
        "failed with Authorization: Bearer sk-abcdef1234567890";
    // NOT-A-SECRET: fixture string, never a live credential
    result.logs = {"sent api_key=sk-zyxwvu9876543210"};

    const std::string encoded = FailureContext{}.build(result).dump();
    EXPECT_EQ(encoded.find("abcdef1234567890"), std::string::npos);
    EXPECT_EQ(encoded.find("zyxwvu9876543210"), std::string::npos);
}

TEST(FailureContext, IsBounded) {
    TestResult result = TestResult::make("api", "t", TestStatus::Failed);
    result.errorDetail = std::string(200000, 'x');
    for (int i = 0; i < 500; ++i) {
        result.logs.push_back(std::string(500, 'y'));
    }

    const std::size_t size = FailureContext{}.build(result).dump().size();
    // One enormous failure must not turn into an enormous bill.
    EXPECT_LT(size, 64 * 1024U);
}

TEST(FailureContext, GpuMockFlagSurvivesDistillation) {
    TestResult result = TestResult::make("gpu", "t", TestStatus::Failed);
    json::Value gpu = json::Value::object();
    gpu.set("available", true);
    gpu.set("is_mock_data", true);
    gpu.set("summary", "[MOCK TEST DATA] 1x MOCK-GPU");
    result.diagnostics.set("gpu", gpu);

    const json::Value context = FailureContext{}.build(result);
    EXPECT_TRUE(context.path("diagnostics.gpu.is_mock_data")->asBool());
}

TEST(FailureContext, RunContextExplainsHowMuchIsBroken) {
    TestRun run;
    run.runId = "run-1";
    run.hostname = "host";
    run.workers = 4;
    run.results.push_back(TestResult::make("api", "a", TestStatus::Passed));
    run.results.push_back(TestResult::make("api", "b", TestStatus::Failed));

    const FailureContext builder;
    const json::Value context = builder.withRunContext(builder.build(run.results[1]), run);

    // One red test among many green ones is a different problem from all red.
    EXPECT_EQ(context.path("run.run_totals.total")->asInt(), 2);
    EXPECT_EQ(context.path("run.run_totals.passed")->asInt(), 1);
    EXPECT_EQ(context.path("run.workers")->asInt(), 4);
}
