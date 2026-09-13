/// Smoke suite — fast, dependency-free checks that the framework itself is
/// working. These run in milliseconds and are what `testforge run --suite
/// smoke` is for: if any of them fails, nothing else is worth running.

#include "testforge/core/Json.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/testing/ShortAssertions.hpp"
#include "testforge/testing/TestRegistry.hpp"

#include <string>
#include <thread>
#include <vector>

using namespace testforge;

TESTFORGE_TEST_OPTS(smoke,
                    framework_alive,
                    TestOptions{}
                        .describe("The runner can execute a test and record a result.")
                        .withTags({"smoke", "fast"})) {
    ctx.log().info("smoke test running");
    ASSERT_TRUE(true);
    ASSERT_EQ(2 + 2, 4);
    ctx.addMetadata("checked", "arithmetic");
}

TESTFORGE_TEST_OPTS(smoke,
                    assertions_produce_diagnostics,
                    TestOptions{}
                        .describe("Assertion helpers compare values correctly across types.")
                        .withTags({"smoke", "fast"})) {
    const int actual = 42;
    ASSERT_EQ(actual, 42);
    ASSERT_NE(actual, 41);
    ASSERT_LT(actual, 100);
    ASSERT_LE(actual, 42);
    ASSERT_GT(actual, 1);
    ASSERT_GE(actual, 42);

    // Signed/unsigned comparison: plain C++ would make this one false.
    const std::size_t size = 10;
    ASSERT_LT(-1, size);

    const std::string text = "TestForge validates responses";
    ASSERT_CONTAINS(text, "validates");
    ASSERT_NOT_CONTAINS(text, "nonsense");
    ASSERT_MATCHES(text, "TestForge*responses");
}

TESTFORGE_TEST_OPTS(smoke,
                    configuration_is_loaded,
                    TestOptions{}
                        .describe("The test context exposes the effective configuration.")
                        .withTags({"smoke", "config"})) {
    ASSERT_FALSE(ctx.config().api.baseUrl.empty());
    ASSERT_GT(ctx.config().execution.defaultTimeoutMs, 0);
    ASSERT_GE(ctx.config().effectiveWorkers(), 1);
    ctx.addMetadata("base_url", ctx.config().api.baseUrl);
    ctx.addMetadata("workers", ctx.config().effectiveWorkers());
}

TESTFORGE_TEST_OPTS(smoke,
                    json_round_trip,
                    TestOptions{}
                        .describe("The bundled JSON parser round-trips a document exactly.")
                        .withTags({"smoke", "json"})) {
    const std::string source =
        R"({"name":"testforge","version":9,"ratio":0.5,"tags":["a","b"],"nested":{"ok":true}})";

    const json::Value parsed = json::parse(source);
    ASSERT_TRUE(parsed.isObject());
    ASSERT_JSON_EQ(parsed, "name", "testforge");
    ASSERT_JSON_EQ(parsed, "version", 9);
    ASSERT_JSON_FIELD(parsed, "nested.ok");
    ASSERT_EQ(parsed.at("tags").size(), std::size_t{2});

    // Re-parsing the serialised form must produce an equal document.
    const json::Value again = json::parse(parsed.dump());
    ASSERT_TRUE(again == parsed);
}

TESTFORGE_TEST_OPTS(smoke,
                    cancellation_is_observed,
                    TestOptions{}
                        .describe("A test can poll its cancellation token without blocking.")
                        .withTags({"smoke", "concurrency"})
                        .withTimeout(5000)) {
    ASSERT_FALSE(ctx.cancellation().isCancelled());
    ctx.throwIfCancelled();

    // sleepFor returns true when the whole duration elapsed uninterrupted.
    const bool slept = ctx.sleepFor(Milliseconds{10});
    ASSERT_TRUE(slept);
    ASSERT_FALSE(ctx.cancellation().isCancelled());
}

TESTFORGE_TEST_OPTS(smoke,
                    string_utilities,
                    TestOptions{}
                        .describe("Glob matching and redaction behave as documented.")
                        .withTags({"smoke", "fast"})) {
    ASSERT_TRUE(strings::globMatch("api.*", "api.health_check"));
    ASSERT_TRUE(strings::globMatch("*health*", "api.health_check"));
    ASSERT_FALSE(strings::globMatch("gpu.*", "api.health_check"));

    // Redaction is applied to everything that reaches a log or a report.
    const std::string redacted = strings::redactSecrets(
        // NOT-A-SECRET: fixture string, never a live credential
        "Authorization: Bearer sk-abcdef1234567890");
    // credential
    ASSERT_NOT_CONTAINS(redacted, "abcdef1234567890");
    ASSERT_CONTAINS(redacted, "REDACTED");
}
