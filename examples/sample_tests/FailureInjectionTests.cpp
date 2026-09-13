/// Failure-injection suite — tests that are SUPPOSED to fail.
///
/// A test tool that only ever demonstrates green runs has demonstrated
/// nothing. Each test here breaks in a specific, controlled way, and the point
/// is to show that TestForge notices, classifies it correctly, captures the
/// evidence, persists it, and reports it usefully.
///
/// This suite is excluded from a bare `testforge run` (see
/// execution.exclude_suites_by_default in config/default.json), so the default
/// run stays green. Run it deliberately:
///
///   testforge run --suite failure_injection
///
/// Expected outcome: every test fails or errors, each with the failure
/// category named in its description. scripts/demo.sh asserts exactly that.

#include "testforge/core/Exceptions.hpp"
#include "testforge/core/Json.hpp"
#include "testforge/core/Process.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/testing/ApiTestCase.hpp"
#include "testforge/testing/ShortAssertions.hpp"
#include "testforge/testing/TestRegistry.hpp"

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace testforge;

// ---------------------------------------------------------------------------
// ASSERTION_FAILURE — the system under test produced the wrong value
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(failure_injection,
                    assertion_equality,
                    TestOptions{}
                        .describe("EXPECTED FAILED / ASSERTION_FAILURE: two values differ.")
                        .withTags({"failure-injection", "assertion"})) {
    const int computed = 6 * 7;
    ctx.log().info("about to compare a computed value against a wrong expectation");
    ctx.addNote("this failure is intentional; it demonstrates assertion diagnostics");

    // The failure message shows both sides and the source location.
    ASSERT_EQ(computed, 43);
}

TESTFORGE_TEST_OPTS(failure_injection,
                    assertion_on_text,
                    TestOptions{}
                        .describe("EXPECTED FAILED / ASSERTION_FAILURE: substring absent.")
                        .withTags({"failure-injection", "assertion"})) {
    const std::string body = R"({"status":"degraded","detail":"cache unavailable"})";
    ctx.addMetadata("response_excerpt", body);
    ASSERT_CONTAINS(body, "\"status\":\"ok\"");
}

TESTFORGE_TEST_OPTS(failure_injection,
                    multiple_soft_assertions,
                    TestOptions{}
                        .describe("EXPECTED FAILED / ASSERTION_FAILURE: three problems reported "
                                  "together, not just the first.")
                        .withTags({"failure-injection", "assertion"})) {
    SoftAssertionScope soft;

    const int statusCode = 503;
    const std::string contentType = "text/html";
    const std::int64_t latencyMs = 4200;

    ASSERT_EQ(statusCode, 200);
    ASSERT_EQ(contentType, std::string("application/json"));
    ASSERT_LT(latencyMs, 1000);

    // Collecting them means one run tells the whole story instead of three.
    soft.verify();
}

// ---------------------------------------------------------------------------
// TIMEOUT — the test did not finish in time
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(failure_injection,
                    cooperative_timeout,
                    TestOptions{}
                        .describe("EXPECTED TIMEOUT / TIMEOUT: a well-behaved test that "
                                  "notices cancellation and unwinds promptly.")
                        .withTags({"failure-injection", "timeout"})
                        .withTimeout(500)) {
    ctx.log().info("starting a long loop that checks for cancellation");
    for (int i = 0; i < 200; ++i) {
        // This is what "cooperative" means: the test asks whether it should
        // stop, so the worker is released almost immediately at the deadline.
        ctx.throwIfCancelled();
        if (!ctx.sleepFor(Milliseconds{50})) {
            ctx.log().warn("cancellation observed during sleep", {{"iteration", i}});
            ctx.throwIfCancelled();
        }
    }
    TF_FAIL("the loop should have been cancelled long before this line");
}

TESTFORGE_TEST_OPTS(failure_injection,
                    uncooperative_timeout,
                    TestOptions{}
                        .describe("EXPECTED TIMEOUT / TIMEOUT: a test that ignores cancellation; "
                                  "TestForge reports it rather than killing the thread.")
                        .withTags({"failure-injection", "timeout"})
                        .withTimeout(400)) {
    // Deliberately uses std::this_thread::sleep_for instead of ctx.sleepFor,
    // so there is no cancellation point. TestForge stops waiting, records
    // TIMEOUT, and reports the abandoned thread at shutdown — see
    // docs/concurrency.md on why it does not force-terminate it.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ctx.log().info("this line runs after the deadline has already been reported");
}

// ---------------------------------------------------------------------------
// NETWORK_FAILURE — the system under test was unreachable
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(failure_injection,
                    connection_refused,
                    TestOptions{}
                        .describe("EXPECTED ERROR / NETWORK_FAILURE: nothing is listening on "
                                  "the target port.")
                        .withTags({"failure-injection", "network"})
                        .withTimeout(15000)) {
    net::HttpClientOptions options;
    options.connectTimeout = Milliseconds{1500};
    options.requestTimeout = Milliseconds{3000};
    // Loopback on a port nothing binds: a refusal, not a hang.
    options.policy.blockLoopback = false;

    net::HttpClient client(options);
    const net::HttpResponse response =
        client.get("http://127.0.0.1:9/definitely-not-listening", {});

    ctx.addMetadata("transport_error", response.errorMessage);
    ctx.addMetadata("http_url", response.requestUrl);

    ASSERT_TRUE(response.transportError);
    // Throwing NetworkError is what puts this in the right triage bucket; an
    // assertion failure here would blame the service for not existing.
    throw NetworkError("the system under test refused the connection: " + response.errorMessage);
}

// ---------------------------------------------------------------------------
// APPLICATION_FAILURE — the service answered, and answered badly
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(failure_injection,
                    server_error_is_application_failure,
                    TestOptions{}
                        .describe("EXPECTED FAILED / APPLICATION_FAILURE: the sample service's "
                                  "/boom endpoint returns 500, and the classifier promotes the "
                                  "assertion failure accordingly.")
                        .withTags({"failure-injection", "application"})
                        .withTimeout(15000)) {
    ApiClient api(ctx);
    api.skipUnlessReachable("/health");

    const net::HttpResponse response = api.get("/boom");
    ctx.log().error("service returned an error status", {{"status", response.statusCode}});

    // The assertion fails; the second classification pass sees http_status 500
    // in the metadata and reclassifies from ASSERTION_FAILURE to
    // APPLICATION_FAILURE, because the service is at fault, not the test.
    ASSERT_STATUS_CODE(response, 200);
}

TESTFORGE_TEST_OPTS(failure_injection,
                    not_found_is_a_plain_assertion_failure,
                    TestOptions{}
                        .describe("EXPECTED FAILED / ASSERTION_FAILURE: a 404 stays an "
                                  "assertion failure, because 4xx is not the server's fault.")
                        .withTags({"failure-injection", "application"})
                        .withTimeout(15000)) {
    ApiClient api(ctx);
    api.skipUnlessReachable("/health");

    const net::HttpResponse response = api.get("/no-such-endpoint");
    ASSERT_STATUS_CODE(response, 200);
}

// ---------------------------------------------------------------------------
// DEPENDENCY_FAILURE — something the test needs is not installed
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(failure_injection,
                    missing_dependency,
                    TestOptions{}
                        .describe("EXPECTED ERROR / DEPENDENCY_FAILURE: a required binary is "
                                  "absent.")
                        .withTags({"failure-injection", "dependency"})) {
    const std::string tool = "testforge-nonexistent-tool";
    const ProcessResult result = ProcessRunner::run(tool, {"--version"});

    ctx.addMetadata("missing_dependency", tool);
    ctx.addMetadata("launch_error", result.launchError);

    ASSERT_FALSE(result.started);
    throw DependencyError("required tool '" + tool + "' is not installed: " + result.launchError);
}

// ---------------------------------------------------------------------------
// CONFIGURATION_FAILURE and ENVIRONMENT_FAILURE
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(failure_injection,
                    invalid_configuration,
                    TestOptions{}
                        .describe("EXPECTED ERROR / CONFIGURATION_FAILURE: a required setting "
                                  "is missing.")
                        .withTags({"failure-injection", "configuration"})) {
    const json::Value* setting = ctx.configValue("failure_injection.required_setting");
    if (setting == nullptr) {
        throw ConfigurationError(
            "custom.failure_injection.required_setting is not set; this test demonstrates how a "
            "configuration problem is reported separately from a test failure");
    }
    ASSERT_TRUE(setting->isString());
}

TESTFORGE_TEST_OPTS(failure_injection,
                    environment_failure,
                    TestOptions{}
                        .describe("EXPECTED ERROR / ENVIRONMENT_FAILURE: an unwritable path.")
                        .withTags({"failure-injection", "environment"})) {
    const std::string path = "/proc/testforge-cannot-write-here";
    ctx.addMetadata("path", path);
    throw EnvironmentError("cannot write to " + path + ": permission denied");
}

// ---------------------------------------------------------------------------
// Uncontrolled failures — what happens when a test misbehaves
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(failure_injection,
                    unexpected_std_exception,
                    TestOptions{}
                        .describe("EXPECTED ERROR: a plain std::exception escapes the test "
                                  "body and is classified from its message.")
                        .withTags({"failure-injection", "robustness"})) {
    ctx.log().info("about to throw a library exception TestForge knows nothing about");
    throw std::out_of_range("vector index 12 is out of range for a container of size 3");
}

TESTFORGE_TEST_OPTS(failure_injection,
                    non_std_exception,
                    TestOptions{}
                        .describe("EXPECTED ERROR / UNKNOWN: an object that does not derive "
                                  "from std::exception still cannot crash the run.")
                        .withTags({"failure-injection", "robustness"})) {
    ctx.log().warn("throwing a non-std exception; the runner should survive it");
    throw 42;  // NOLINT(hicpp-exception-baseclass) — deliberate, see above
}

TESTFORGE_TEST_OPTS(failure_injection,
                    failure_in_setup,
                    TestOptions{}
                        .describe("EXPECTED ERROR: a fixture problem is reported as an error, "
                                  "not as a test failure.")
                        .withTags({"failure-injection", "robustness"})) {
    // FunctionTestCase has no setUp of its own, so this simulates the same
    // situation: the failure happens before any behaviour has been checked.
    ctx.addNote("simulating a fixture that could not be prepared");
    throw ResourceError("could not allocate the 4 GiB buffer this test needs");
}

// ---------------------------------------------------------------------------
// A passing test, on purpose
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(failure_injection,
                    control_test_passes,
                    TestOptions{}
                        .describe("EXPECTED PASSED: the control. If this fails, the suite is "
                                  "broken rather than demonstrating anything.")
                        .withTags({"failure-injection", "control"})) {
    ASSERT_TRUE(true);
    ctx.addMetadata("role", "control");
}
