/// Tests for the core utilities: strings, clock, ids, status, config, logging.

#include "testforge/core/Clock.hpp"
#include "testforge/core/Config.hpp"
#include "testforge/core/Environment.hpp"
#include "testforge/core/Exceptions.hpp"
#include "testforge/core/Ids.hpp"
#include "testforge/core/Interrupt.hpp"
#include "testforge/core/Logger.hpp"
#include "testforge/core/Status.hpp"
#include "testforge/core/StringUtils.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <thread>

using namespace testforge;

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

TEST(Strings, TrimAndCase) {
    EXPECT_EQ(strings::trim("  hello  "), "hello");
    EXPECT_EQ(strings::trim("\t\n x \r\n"), "x");
    EXPECT_EQ(strings::trim("   "), "");
    EXPECT_EQ(strings::toLower("MiXeD"), "mixed");
    EXPECT_EQ(strings::toUpper("MiXeD"), "MIXED");
    EXPECT_TRUE(strings::equalsIgnoreCase("Content-Type", "content-type"));
    EXPECT_FALSE(strings::equalsIgnoreCase("abc", "abcd"));
}

TEST(Strings, SplitAndJoin) {
    EXPECT_EQ(strings::split("a,b,c", ',').size(), 3U);
    EXPECT_EQ(strings::split("a,,c", ',').size(), 3U);
    EXPECT_EQ(strings::split("a,,c", ',', true).size(), 2U);
    EXPECT_EQ(strings::split("", ',').size(), 1U);
    EXPECT_EQ(strings::join({"a", "b", "c"}, "-"), "a-b-c");
    EXPECT_EQ(strings::join({}, "-"), "");
}

TEST(Strings, SplitLinesHandlesCrlf) {
    const std::vector<std::string> lines = strings::splitLines("a\r\nb\nc\n");
    ASSERT_EQ(lines.size(), 3U);
    EXPECT_EQ(lines[0], "a");
    EXPECT_EQ(lines[2], "c");
}

TEST(Strings, GlobMatching) {
    EXPECT_TRUE(strings::globMatch("*", "anything"));
    EXPECT_TRUE(strings::globMatch("api.*", "api.health"));
    EXPECT_TRUE(strings::globMatch("*health*", "api.health_check"));
    EXPECT_TRUE(strings::globMatch("a?c", "abc"));
    EXPECT_TRUE(strings::globMatch("exact", "exact"));
    EXPECT_FALSE(strings::globMatch("api.*", "gpu.health"));
    EXPECT_FALSE(strings::globMatch("a?c", "ac"));
    EXPECT_TRUE(strings::globMatch("", ""));
    EXPECT_FALSE(strings::globMatch("", "x"));
}

TEST(Strings, GlobDoesNotBlowUpOnPathologicalPatterns) {
    // The iterative matcher exists so this cannot go exponential; if it ever
    // regresses to naive recursion this test will hang rather than pass.
    const std::string pattern = "*a*a*a*a*a*a*a*a*b";
    const std::string text(64, 'a');
    EXPECT_FALSE(strings::globMatch(pattern, text));
}

TEST(Strings, TruncateMarksTheCut) {
    EXPECT_EQ(strings::truncate("short", 100), "short");
    const std::string cut = strings::truncate(std::string(500, 'x'), 50);
    EXPECT_EQ(cut.size(), 50U);
    EXPECT_NE(cut.find("truncated"), std::string::npos);
}

TEST(Strings, RedactsBearerTokens) {
    const std::string redacted = strings::redactSecrets(
        // NOT-A-SECRET: fixture string, never a live credential
        "Authorization: Bearer sk-abcdef1234567890xyz");
    EXPECT_EQ(redacted.find("abcdef1234567890"), std::string::npos);
    EXPECT_NE(redacted.find("REDACTED"), std::string::npos);
}

TEST(Strings, RedactsProviderKeyShapes) {
    for (const std::string sample :
         // NOT-A-SECRET: fixture string, never a live credential
         {"sk-abcdefghijklmnop",
          "ghp_abcdefghijklmnop",
          "xoxb-abcdefghijklmnop",
          // NOT-A-SECRET: fixture string, never a live credential
          "AKIAABCDEFGHIJKLMNOP"}) {
        const std::string redacted = strings::redactSecrets("token is " + sample);
        EXPECT_NE(redacted.find("REDACTED"), std::string::npos) << sample;
        EXPECT_EQ(redacted.find(sample), std::string::npos) << sample;
    }
}

TEST(Strings, RedactsKeyValuePairs) {
    const std::string redacted = strings::redactSecrets(R"({"password":"hunter2","user":"alice"})");
    EXPECT_EQ(redacted.find("hunter2"), std::string::npos);
    // Non-sensitive fields are left alone.
    EXPECT_NE(redacted.find("alice"), std::string::npos);
}

TEST(Strings, RedactionIsIdempotent) {
    // A regression test with history: an earlier implementation recursed after
    // each substitution and looped forever the second time it met its own
    // mask, taking the process down with a stack overflow.
    const std::string once = strings::redactSecrets(
        // NOT-A-SECRET: fixture string, never a live credential
        "Authorization: Bearer sk-abcdef1234567890");
    // credential
    const std::string twice = strings::redactSecrets(once);
    const std::string thrice = strings::redactSecrets(twice);
    EXPECT_EQ(once, twice);
    EXPECT_EQ(twice, thrice);
}

TEST(Strings, RedactionLeavesOrdinaryTextAlone) {
    const std::string text = "GET /users/1 returned 200 in 14ms";
    EXPECT_EQ(strings::redactSecrets(text), text);
}

TEST(Strings, ShortPrefixesAreNotMistakenForKeys) {
    // "sk-1" is far too short to be a key; masking it would be noise.
    EXPECT_EQ(strings::redactSecrets("model sk-1 here"), "model sk-1 here");
}

TEST(Strings, SensitiveNameDetection) {
    EXPECT_TRUE(strings::isSensitiveName("OPENAI_API_KEY"));
    EXPECT_TRUE(strings::isSensitiveName("db_password"));
    EXPECT_TRUE(strings::isSensitiveName("Authorization"));
    EXPECT_FALSE(strings::isSensitiveName("PATH"));
    EXPECT_FALSE(strings::isSensitiveName("hostname"));
}

TEST(Strings, EscapingForXmlAndHtml) {
    EXPECT_EQ(strings::escapeXml("<a href=\"x\">&</a>"),
              "&lt;a href=&quot;x&quot;&gt;&amp;&lt;/a&gt;");
    EXPECT_NE(strings::escapeHtml("<script>").find("&lt;script&gt;"), std::string::npos);
}

TEST(Strings, Parsing) {
    std::int64_t number = 0;
    EXPECT_TRUE(strings::parseInt("42", number));
    EXPECT_EQ(number, 42);
    EXPECT_TRUE(strings::parseInt("  -7 ", number));
    EXPECT_EQ(number, -7);
    EXPECT_FALSE(strings::parseInt("42abc", number));
    EXPECT_FALSE(strings::parseInt("", number));

    bool flag = false;
    EXPECT_TRUE(strings::parseBool("yes", flag));
    EXPECT_TRUE(flag);
    EXPECT_TRUE(strings::parseBool("OFF", flag));
    EXPECT_FALSE(flag);
    EXPECT_FALSE(strings::parseBool("maybe", flag));
}

TEST(Strings, DurationParsing) {
    std::int64_t millis = 0;
    EXPECT_TRUE(strings::parseDurationMillis("500", millis));
    EXPECT_EQ(millis, 500);
    EXPECT_TRUE(strings::parseDurationMillis("2s", millis));
    EXPECT_EQ(millis, 2000);
    EXPECT_TRUE(strings::parseDurationMillis("1.5s", millis));
    EXPECT_EQ(millis, 1500);
    EXPECT_TRUE(strings::parseDurationMillis("3m", millis));
    EXPECT_EQ(millis, 180000);
    EXPECT_FALSE(strings::parseDurationMillis("soon", millis));
    EXPECT_FALSE(strings::parseDurationMillis("-5s", millis));
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

TEST(Clock, Iso8601RoundTrip) {
    const TimePoint now = WallClock::now();
    const std::string text = toIso8601(now);

    EXPECT_EQ(text.size(), 24U);  // 2026-09-12T06:31:32.417Z
    EXPECT_EQ(text.back(), 'Z');
    EXPECT_NE(text.find('T'), std::string::npos);

    // Millisecond precision, so the round trip is exact to the millisecond.
    EXPECT_EQ(toEpochMillis(fromIso8601(text)), toEpochMillis(now));
}

TEST(Clock, Iso8601RejectsRubbish) {
    EXPECT_EQ(toEpochMillis(fromIso8601("not a timestamp")), 0);
    EXPECT_EQ(toEpochMillis(fromIso8601("")), 0);
}

TEST(Clock, EpochMillisRoundTrip) {
    const std::int64_t millis = 1757658692417LL;
    EXPECT_EQ(toEpochMillis(fromEpochMillis(millis)), millis);
}

TEST(Clock, DurationFormatting) {
    EXPECT_EQ(formatDuration(Milliseconds{0}), "0ms");
    EXPECT_EQ(formatDuration(Milliseconds{999}), "999ms");
    EXPECT_EQ(formatDuration(Milliseconds{1500}), "1.500s");
    EXPECT_NE(formatDuration(Milliseconds{125000}).find('m'), std::string::npos);
}

TEST(Clock, StopwatchMeasuresForwards) {
    const Stopwatch watch;
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    EXPECT_GE(watch.elapsed().count(), 10);
    EXPECT_LT(watch.elapsed().count(), 5000);
}

TEST(Clock, MillisOfIsAPlainInteger) {
    EXPECT_EQ(millisOf(Milliseconds{1234}), 1234);
}

// ---------------------------------------------------------------------------
// Ids
// ---------------------------------------------------------------------------

TEST(Ids, HexIdsAreUniqueAndWellFormed) {
    const std::string a = ids::generateHexId(16);
    const std::string b = ids::generateHexId(16);
    EXPECT_EQ(a.size(), 32U);
    EXPECT_NE(a, b);
    for (const char c : a) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << c;
    }
}

TEST(Ids, TestIdIsDeterministic) {
    // History and flake analysis depend on the same test producing the same id
    // across runs and across machines.
    EXPECT_EQ(ids::testId("api", "health"), ids::testId("api", "health"));
    EXPECT_NE(ids::testId("api", "health"), ids::testId("gpu", "health"));
    EXPECT_EQ(ids::testId("api", "health").size(), 16U);
}

TEST(Ids, Fnv1aIsStable) {
    EXPECT_EQ(ids::fnv1a64("testforge"), ids::fnv1a64("testforge"));
    EXPECT_NE(ids::fnv1a64("a"), ids::fnv1a64("b"));
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

TEST(Status, StringRoundTrip) {
    for (const TestStatus status : allTestStatuses()) {
        const auto parsed = testStatusFromString(toString(status));
        ASSERT_TRUE(parsed.has_value()) << toString(status);
        EXPECT_EQ(*parsed, status);
    }
    for (const FailureCategory category : allFailureCategories()) {
        const auto parsed = failureCategoryFromString(toString(category));
        ASSERT_TRUE(parsed.has_value()) << toString(category);
        EXPECT_EQ(*parsed, category);
    }
}

TEST(Status, ParsingIsCaseInsensitiveAndRejectsUnknown) {
    EXPECT_EQ(testStatusFromString("passed"), TestStatus::Passed);
    EXPECT_FALSE(testStatusFromString("nonsense").has_value());
}

TEST(Status, FailureClassification) {
    EXPECT_FALSE(isFailure(TestStatus::Passed));
    EXPECT_FALSE(isFailure(TestStatus::Skipped));
    EXPECT_TRUE(isFailure(TestStatus::Failed));
    EXPECT_TRUE(isFailure(TestStatus::Error));
    EXPECT_TRUE(isFailure(TestStatus::Timeout));
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

TEST(Config, DefaultsAreUsable) {
    const Config config;
    EXPECT_GE(config.effectiveWorkers(), 1);
    EXPECT_GT(config.execution.defaultTimeoutMs, 0);
    EXPECT_FALSE(config.api.baseUrl.empty());
    EXPECT_FALSE(config.ai.enabled) << "AI must be opt-in";
    EXPECT_EQ(config.server.host, "127.0.0.1") << "the server must not bind publicly by default";
    EXPECT_NO_THROW(config.validate());
}

TEST(Config, FromJsonAppliesOverrides) {
    const json::Value document = json::parse(R"({
        "execution": {"workers": 7, "default_timeout_ms": 1234, "fail_fast": true,
                      "exclude_suites_by_default": ["a", "b"]},
        "database": {"enabled": false, "path": "/tmp/x.db"},
        "api": {"base_url": "http://example.test:9000", "timeout_ms": 999},
        "ai": {"enabled": true, "model": "test-model"},
        "custom": {"nested": {"value": 42}}
    })");

    const Config config = Config::fromJson(document);
    EXPECT_EQ(config.execution.workers, 7);
    EXPECT_EQ(config.execution.defaultTimeoutMs, 1234);
    EXPECT_TRUE(config.execution.failFast);
    EXPECT_EQ(config.execution.excludeSuitesByDefault.size(), 2U);
    EXPECT_FALSE(config.database.enabled);
    EXPECT_EQ(config.api.baseUrl, "http://example.test:9000");
    EXPECT_TRUE(config.ai.enabled);
    EXPECT_EQ(config.ai.model, "test-model");

    ASSERT_NE(config.custom.path("nested.value"), nullptr);
    EXPECT_EQ(config.custom.path("nested.value")->asInt(), 42);
}

TEST(Config, PartialJsonKeepsDefaults) {
    const Config config = Config::fromJson(json::parse(R"({"execution":{"workers":3}})"));
    EXPECT_EQ(config.execution.workers, 3);
    EXPECT_EQ(config.execution.defaultTimeoutMs, Config{}.execution.defaultTimeoutMs);
}

TEST(Config, ValidationRejectsNonsense) {
    Config config;
    config.execution.workers = -1;
    EXPECT_THROW(config.validate(), ConfigurationError);

    config = Config{};
    config.server.port = 70000;
    EXPECT_THROW(config.validate(), ConfigurationError);

    config = Config{};
    config.api.baseUrl = "ftp://example.test";
    EXPECT_THROW(config.validate(), ConfigurationError);

    config = Config{};
    config.ai.enabled = true;
    config.ai.endpoint.clear();
    EXPECT_THROW(config.validate(), ConfigurationError);
}

TEST(Config, ValidationReportsEveryProblemAtOnce) {
    Config config;
    config.execution.workers = -1;
    config.server.port = 0;
    try {
        config.validate();
        FAIL() << "expected ConfigurationError";
    } catch (const ConfigurationError& error) {
        const std::string message = error.what();
        // Both problems, so the user fixes them in one pass instead of two.
        EXPECT_NE(message.find("workers"), std::string::npos);
        EXPECT_NE(message.find("port"), std::string::npos);
    }
}

TEST(Config, MissingFileIsAConfigurationError) {
    EXPECT_THROW((void)Config::loadFromFile("/nonexistent/testforge.json"), ConfigurationError);
}

TEST(Config, JsonRoundTrip) {
    Config config;
    config.execution.workers = 5;
    config.reporting.junit = true;
    const Config again = Config::fromJson(config.toJson());
    EXPECT_EQ(again.execution.workers, 5);
    EXPECT_TRUE(again.reporting.junit);
}

TEST(Config, EnvironmentOverridesWin) {
    ::setenv("TESTFORGE_WORKERS", "11", 1);
    ::setenv("TESTFORGE_API_BASE_URL", "http://env.test:1234", 1);

    Config config;
    config.applyEnvironmentOverrides();
    EXPECT_EQ(config.execution.workers, 11);
    EXPECT_EQ(config.api.baseUrl, "http://env.test:1234");

    ::unsetenv("TESTFORGE_WORKERS");
    ::unsetenv("TESTFORGE_API_BASE_URL");
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

TEST(Environment, ReadsAndFallsBack) {
    ::setenv("TESTFORGE_UNIT_TEST_VALUE", "hello", 1);
    EXPECT_EQ(env::getOr("TESTFORGE_UNIT_TEST_VALUE", "fallback"), "hello");
    EXPECT_EQ(env::getOr("TESTFORGE_DEFINITELY_UNSET", "fallback"), "fallback");
    EXPECT_TRUE(env::isSet("TESTFORGE_UNIT_TEST_VALUE"));
    EXPECT_FALSE(env::isSet("TESTFORGE_DEFINITELY_UNSET"));
    ::unsetenv("TESTFORGE_UNIT_TEST_VALUE");
}

TEST(Environment, DiagnosticSubsetRedactsSensitiveNames) {
    ::setenv("TESTFORGE_UNIT_TOKEN", "super-secret-value", 1);
    const std::map<std::string, std::string> snapshot = env::snapshotRedacted();

    const auto found = snapshot.find("TESTFORGE_UNIT_TOKEN");
    ASSERT_NE(found, snapshot.end());
    EXPECT_EQ(found->second, "***REDACTED***");
    ::unsetenv("TESTFORGE_UNIT_TOKEN");
}

TEST(Environment, DotEnvDoesNotOverrideTheRealEnvironment) {
    const std::string path = "/tmp/testforge-unit.env";
    {
        std::ofstream file(path);
        file << "# a comment\n";
        file << "TESTFORGE_DOTENV_NEW=from-file\n";
        file << "export TESTFORGE_DOTENV_QUOTED=\"quoted value\"\n";
        file << "TESTFORGE_DOTENV_EXISTING=from-file\n";
    }
    ::setenv("TESTFORGE_DOTENV_EXISTING", "from-environment", 1);

    const auto applied = env::loadDotEnv(path);
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(env::getOr("TESTFORGE_DOTENV_NEW", ""), "from-file");
    EXPECT_EQ(env::getOr("TESTFORGE_DOTENV_QUOTED", ""), "quoted value");
    // The real environment always wins over a file.
    EXPECT_EQ(env::getOr("TESTFORGE_DOTENV_EXISTING", ""), "from-environment");

    ::unsetenv("TESTFORGE_DOTENV_NEW");
    ::unsetenv("TESTFORGE_DOTENV_QUOTED");
    ::unsetenv("TESTFORGE_DOTENV_EXISTING");
    std::remove(path.c_str());
}

TEST(Environment, MissingDotEnvIsNotAnError) {
    EXPECT_FALSE(env::loadDotEnv("/nonexistent/.env").has_value());
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

namespace {

/// Swaps in a memory sink for the duration of a test and restores the previous
/// sinks afterwards, so log configuration cannot leak between tests.
class CapturedLogging {
 public:
    CapturedLogging() : sink_(std::make_shared<MemoryLogSink>()) {
        previousLevel_ = LogManager::instance().level();
        LogManager::instance().setLevel(LogLevel::Trace);
        previous_ = LogManager::instance().replaceSinks({sink_});
    }

    ~CapturedLogging() {
        LogManager::instance().replaceSinks(previous_);
        LogManager::instance().setLevel(previousLevel_);
    }

    CapturedLogging(const CapturedLogging&) = delete;
    CapturedLogging& operator=(const CapturedLogging&) = delete;

    [[nodiscard]] MemoryLogSink& sink() const { return *sink_; }

 private:
    std::shared_ptr<MemoryLogSink> sink_;
    std::vector<std::shared_ptr<LogSink>> previous_;
    LogLevel previousLevel_ = LogLevel::Info;
};

}  // namespace

TEST(Logging, LevelsAreOrdered) {
    EXPECT_EQ(logLevelFromString("debug"), LogLevel::Debug);
    EXPECT_EQ(logLevelFromString("WARNING"), LogLevel::Warn);
    EXPECT_FALSE(logLevelFromString("chatty").has_value());
}

TEST(Logging, RecordsReachTheSink) {
    const CapturedLogging captured;
    const Logger logger("unit");
    logger.info("hello", {{"key", "value"}});

    const std::vector<LogRecord> records = captured.sink().records();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].component, "unit");
    EXPECT_EQ(records[0].message, "hello");
    ASSERT_EQ(records[0].fields.size(), 1U);
    EXPECT_EQ(records[0].fields[0].first, "key");
}

TEST(Logging, LevelThresholdSuppresses) {
    const CapturedLogging captured;
    LogManager::instance().setLevel(LogLevel::Warn);

    const Logger logger("unit");
    logger.debug("suppressed");
    logger.info("suppressed");
    logger.error("kept");

    ASSERT_EQ(captured.sink().records().size(), 1U);
    EXPECT_EQ(captured.sink().records()[0].message, "kept");
}

TEST(Logging, MessagesAndFieldsAreRedacted) {
    const CapturedLogging captured;
    const Logger logger("unit");
    logger.info(
        // NOT-A-SECRET: fixture string, never a live credential
        "using Bearer sk-abcdef1234567890",
        {{"api_key",
          // NOT-A-SECRET: fixture string, never a live credential
          "sk-zyxwvu9876543210"}});

    const std::vector<LogRecord> records = captured.sink().records();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].message.find("abcdef1234567890"), std::string::npos);
    ASSERT_EQ(records[0].fields.size(), 1U);
    EXPECT_EQ(records[0].fields[0].second.asString().find("zyxwvu9876543210"), std::string::npos);
}

TEST(Logging, ScopedCaptureIsPerThread) {
    const CapturedLogging captured;
    const Logger logger("unit");

    std::vector<std::string> otherThreadLines;
    {
        ScopedLogCapture capture;
        logger.info("on this thread");

        // A capture on one thread must not swallow another thread's records:
        // that property is what gives each test its own clean log tail.
        std::thread other([&logger, &otherThreadLines] {
            ScopedLogCapture innerCapture;
            logger.info("on the other thread");
            otherThreadLines = innerCapture.lines();
        });
        other.join();

        const std::vector<std::string> lines = capture.lines();
        ASSERT_EQ(lines.size(), 1U);
        EXPECT_NE(lines[0].find("on this thread"), std::string::npos);
    }

    ASSERT_EQ(otherThreadLines.size(), 1U);
    EXPECT_NE(otherThreadLines[0].find("on the other thread"), std::string::npos);
}

TEST(Logging, JsonRenderingIsParseable) {
    LogRecord record;
    record.timestamp = WallClock::now();
    record.level = LogLevel::Error;
    record.component = "unit";
    record.message = "something went wrong";
    record.test = "suite.test";
    record.fields.emplace_back("status", json::Value(500));

    const json::Value document = LogManager::toJson(record);
    EXPECT_EQ(document.at("level").asString(), "ERROR");
    EXPECT_EQ(document.at("component").asString(), "unit");
    EXPECT_EQ(document.path("fields.status")->asInt(), 500);
    EXPECT_NO_THROW((void)json::parse(document.dump()));
}

TEST(Logging, ChildAndTestBinding) {
    const CapturedLogging captured;
    Logger("parent").child("child").forTest("suite.name").warn("message");

    const std::vector<LogRecord> records = captured.sink().records();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].component, "parent.child");
    EXPECT_EQ(records[0].test, "suite.name");
}

// ---------------------------------------------------------------------------
// Interrupt
// ---------------------------------------------------------------------------
//
// These exist because the original code installed SIGINT/SIGTERM handlers and
// then never read the flag they set. That is worse than installing nothing:
// replacing the default disposition without honouring it means Ctrl-C stops
// working. A test that raises the signal and asserts the flag is observable is
// the only thing that would have caught it.

TEST(Interrupt, StartsClear) {
    interrupt::reset();
    EXPECT_FALSE(interrupt::requested());
    EXPECT_EQ(interrupt::signalNumber(), 0);
}

TEST(Interrupt, ObservesSigint) {
    interrupt::reset();
    interrupt::installHandlers();

    std::raise(SIGINT);

    EXPECT_TRUE(interrupt::requested());
    EXPECT_EQ(interrupt::signalNumber(), SIGINT);
    interrupt::reset();
}

TEST(Interrupt, ObservesSigterm) {
    interrupt::reset();
    interrupt::installHandlers();

    std::raise(SIGTERM);

    EXPECT_TRUE(interrupt::requested());
    EXPECT_EQ(interrupt::signalNumber(), SIGTERM);
    interrupt::reset();
}

TEST(Interrupt, KeepsTheFirstSignal) {
    // The first signal is the interesting one. A SIGTERM arriving after a
    // SIGINT should not rewrite the reason the process is shutting down.
    interrupt::reset();
    interrupt::installHandlers();

    std::raise(SIGINT);
    std::raise(SIGTERM);

    EXPECT_EQ(interrupt::signalNumber(), SIGINT);
    interrupt::reset();
}

TEST(Interrupt, InstallIsIdempotent) {
    interrupt::reset();
    interrupt::installHandlers();
    interrupt::installHandlers();

    std::raise(SIGTERM);

    EXPECT_TRUE(interrupt::requested());
    interrupt::reset();
}

TEST(Interrupt, IsVisibleFromAnotherThread) {
    // The flag is written by a handler on whatever thread the kernel picked
    // and read by whichever thread is polling it, so the ordering has to be
    // acquire/release rather than relaxed.
    interrupt::reset();
    interrupt::installHandlers();

    std::atomic<bool> seen{false};
    std::thread watcher([&seen] {
        for (int i = 0; i < 2000 && !seen.load(std::memory_order_acquire); ++i) {
            if (interrupt::requested()) {
                seen.store(true, std::memory_order_release);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::raise(SIGTERM);
    watcher.join();

    EXPECT_TRUE(seen.load(std::memory_order_acquire));
    interrupt::reset();
}
