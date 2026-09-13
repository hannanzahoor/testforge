/// Tests for the assertion engine, the registry and the selector.
///
/// Note that these use GoogleTest's ASSERT_*/EXPECT_*, not TestForge's. That
/// separation is deliberate: the thing under test cannot also be the thing
/// doing the checking.

#include "testforge/core/Exceptions.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/testing/Assertions.hpp"
#include "testforge/testing/TestRegistry.hpp"
#include "testforge/testing/TestSelector.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace testforge;

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------

TEST(Assertions, PassingChecksDoNotThrow) {
    EXPECT_NO_THROW(TF_ASSERT_TRUE(true));
    EXPECT_NO_THROW(TF_ASSERT_FALSE(false));
    EXPECT_NO_THROW(TF_ASSERT_EQ(1, 1));
    EXPECT_NO_THROW(TF_ASSERT_NE(1, 2));
    EXPECT_NO_THROW(TF_ASSERT_LT(1, 2));
    EXPECT_NO_THROW(TF_ASSERT_LE(2, 2));
    EXPECT_NO_THROW(TF_ASSERT_GT(2, 1));
    EXPECT_NO_THROW(TF_ASSERT_GE(2, 2));
}

TEST(Assertions, FailingChecksThrowAssertionFailure) {
    EXPECT_THROW(TF_ASSERT_TRUE(false), AssertionFailure);
    EXPECT_THROW(TF_ASSERT_EQ(1, 2), AssertionFailure);
    EXPECT_THROW(TF_ASSERT_LT(2, 1), AssertionFailure);
}

TEST(Assertions, FailureCarriesExpectedAndActual) {
    try {
        TF_ASSERT_EQ(41, 42);
        FAIL() << "expected an AssertionFailure";
    } catch (const AssertionFailure& failure) {
        EXPECT_EQ(failure.expected(), "42");
        EXPECT_EQ(failure.actual(), "41");
        EXPECT_NE(failure.expression().find("41"), std::string::npos);
        EXPECT_TRUE(failure.where().valid());
        EXPECT_EQ(failure.category(), FailureCategory::AssertionFailure);

        // The rendered detail is what a human actually reads.
        const std::string detail = failure.detail();
        EXPECT_NE(detail.find("Expected"), std::string::npos);
        EXPECT_NE(detail.find("Actual"), std::string::npos);
        EXPECT_NE(detail.find("TestingTest.cpp"), std::string::npos);
    }
}

TEST(Assertions, StringsAreQuotedInMessages) {
    // Without quotes, "abc " and "abc" look identical in a failure report.
    try {
        TF_ASSERT_EQ(std::string("abc "), std::string("abc"));
        FAIL() << "expected an AssertionFailure";
    } catch (const AssertionFailure& failure) {
        EXPECT_EQ(failure.actual(), "\"abc \"");
        EXPECT_EQ(failure.expected(), "\"abc\"");
    }
}

TEST(Assertions, MixedSignComparisonIsMathematicallyCorrect) {
    // Plain C++ would make -1 < 1u false. std::cmp_less gives the answer a
    // test author expects.
    const std::size_t size = 10;
    EXPECT_NO_THROW(TF_ASSERT_LT(-1, size));
    EXPECT_NO_THROW(TF_ASSERT_GT(size, -1));
    EXPECT_THROW(TF_ASSERT_EQ(-1, size), AssertionFailure);
}

TEST(Assertions, SubstringAndGlob) {
    EXPECT_NO_THROW(TF_ASSERT_CONTAINS(std::string("hello world"), "world"));
    EXPECT_THROW(TF_ASSERT_CONTAINS(std::string("hello"), "world"), AssertionFailure);
    EXPECT_NO_THROW(TF_ASSERT_NOT_CONTAINS(std::string("hello"), "world"));
    EXPECT_NO_THROW(TF_ASSERT_MATCHES(std::string("api.health"), "api.*"));
    EXPECT_THROW(TF_ASSERT_MATCHES(std::string("gpu.health"), "api.*"), AssertionFailure);
}

TEST(Assertions, LongHaystacksAreTruncatedInTheMessage) {
    const std::string huge(5000, 'x');
    try {
        TF_ASSERT_CONTAINS(huge, "needle");
        FAIL() << "expected an AssertionFailure";
    } catch (const AssertionFailure& failure) {
        // A 5000-character response body in a one-line failure helps nobody.
        EXPECT_LT(failure.actual().size(), 500U);
    }
}

TEST(Assertions, NearComparison) {
    EXPECT_NO_THROW(TF_ASSERT_NEAR(1.0, 1.05, 0.1));
    EXPECT_THROW(TF_ASSERT_NEAR(1.0, 1.5, 0.1), AssertionFailure);
}

TEST(Assertions, JsonHelpers) {
    const json::Value document = json::parse(R"({"a":{"b":1},"list":[1,2]})");
    EXPECT_NO_THROW(TF_ASSERT_JSON_FIELD(document, "a.b"));
    EXPECT_THROW(TF_ASSERT_JSON_FIELD(document, "a.missing"), AssertionFailure);
    EXPECT_NO_THROW(TF_ASSERT_JSON_EQ(document, "a.b", 1));
    EXPECT_THROW(TF_ASSERT_JSON_EQ(document, "a.b", 2), AssertionFailure);
}

TEST(Assertions, StatusCodeMessageNamesTheEndpoint) {
    try {
        assertions::statusCode(500, 200, "GET /users", TESTFORGE_CURRENT_LOCATION());
        FAIL() << "expected an AssertionFailure";
    } catch (const AssertionFailure& failure) {
        EXPECT_EQ(failure.expected(), "200");
        EXPECT_EQ(failure.actual(), "500");
        EXPECT_NE(std::string(failure.what()).find("GET /users"), std::string::npos);
    }
}

TEST(Assertions, ResponseTimeReportsTheOvershoot) {
    try {
        assertions::responseTime(
            Milliseconds{2500}, Milliseconds{1000}, "GET /slow", TESTFORGE_CURRENT_LOCATION());
        FAIL() << "expected an AssertionFailure";
    } catch (const AssertionFailure& failure) {
        EXPECT_NE(failure.actual().find("over budget"), std::string::npos);
    }
}

TEST(Assertions, ThrowsAndNoThrowHelpers) {
    EXPECT_NO_THROW(TF_ASSERT_THROWS(throw std::runtime_error("x"), std::runtime_error));
    EXPECT_THROW(TF_ASSERT_THROWS((void)0, std::runtime_error), AssertionFailure);
    EXPECT_NO_THROW(TF_ASSERT_NO_THROW((void)0));
    EXPECT_THROW(TF_ASSERT_NO_THROW(throw std::runtime_error("x")), AssertionFailure);
}

TEST(Assertions, CountsAreTracked) {
    AssertionEngine::resetCounters();
    TF_ASSERT_TRUE(true);
    TF_ASSERT_EQ(1, 1);
    EXPECT_EQ(AssertionEngine::checksPerformed(), 2U);
}

TEST(SoftAssertions, CollectEveryFailureRatherThanTheFirst) {
    bool threw = false;
    try {
        SoftAssertionScope soft;
        TF_ASSERT_EQ(1, 2);    // fails, does not unwind
        TF_ASSERT_EQ(3, 4);    // also fails
        TF_ASSERT_TRUE(true);  // passes
        EXPECT_EQ(soft.failureCount(), 2U);
        soft.verify();
    } catch (const AssertionFailure& failure) {
        threw = true;
        const std::string message = failure.what();
        EXPECT_NE(message.find("2 assertion(s) failed"), std::string::npos);
        EXPECT_NE(message.find("[1]"), std::string::npos);
        EXPECT_NE(message.find("[2]"), std::string::npos);
    }
    EXPECT_TRUE(threw);
}

TEST(SoftAssertions, VerifyIsSilentWhenEverythingPassed) {
    SoftAssertionScope soft;
    TF_ASSERT_TRUE(true);
    EXPECT_EQ(soft.failureCount(), 0U);
    EXPECT_NO_THROW(soft.verify());
}

TEST(SoftAssertions, DestructorNeverThrows) {
    // Throwing from a destructor during unwinding calls std::terminate; a test
    // that forgets verify() must not take the process down.
    EXPECT_NO_THROW({
        SoftAssertionScope soft;
        TF_ASSERT_EQ(1, 2);
    });
}

TEST(SoftAssertions, ScopesNest) {
    try {
        SoftAssertionScope outer;
        {
            SoftAssertionScope inner;
            TF_ASSERT_EQ(1, 2);
            EXPECT_EQ(inner.failureCount(), 1U);
            EXPECT_EQ(outer.failureCount(), 0U);
            EXPECT_THROW(inner.verify(), AssertionFailure);
        }
        outer.verify();
    } catch (const AssertionFailure&) {
        FAIL() << "the outer scope recorded nothing and should not have thrown";
    }
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

namespace {

/// Registers into a private registry instance so these tests cannot disturb
/// the process-wide catalogue that the example suites live in.
TestMetadata makeMetadata(std::string suite, std::string name, std::vector<std::string> tags = {}) {
    TestMetadata metadata;
    metadata.suite = std::move(suite);
    metadata.name = std::move(name);
    metadata.tags = std::move(tags);
    return metadata;
}

TestFactory trivialFactory(const TestMetadata& metadata) {
    return [metadata]() -> TestCasePtr {
        return std::make_unique<FunctionTestCase>(metadata, [](TestContext&) {});
    };
}

/// RAII guard around the singleton registry: saves nothing, but clears it
/// afterwards so a test that registers cannot leak into the next one.
class ScopedRegistry {
 public:
    ScopedRegistry() { TestRegistry::instance().clear(); }

    ~ScopedRegistry() { TestRegistry::instance().clear(); }

    ScopedRegistry(const ScopedRegistry&) = delete;
    ScopedRegistry& operator=(const ScopedRegistry&) = delete;

    [[nodiscard]] TestRegistry& get() const { return TestRegistry::instance(); }
};

}  // namespace

TEST(Registry, RegisterAndCreate) {
    const ScopedRegistry scoped;
    TestRegistry& registry = scoped.get();

    const TestMetadata metadata = makeMetadata("suite", "name", {"tag"});
    registry.registerTest(metadata, trivialFactory(metadata));

    EXPECT_EQ(registry.size(), 1U);
    EXPECT_TRUE(registry.contains("suite.name"));
    EXPECT_FALSE(registry.contains("suite.other"));

    const TestCasePtr instance = registry.create("suite.name");
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->qualifiedName(), "suite.name");
    EXPECT_EQ(registry.create("nope.nope"), nullptr);
}

TEST(Registry, EachCreateProducesAFreshInstance) {
    const ScopedRegistry scoped;
    const TestMetadata metadata = makeMetadata("suite", "name");
    scoped.get().registerTest(metadata, trivialFactory(metadata));

    // A retry must not inherit state from the attempt that failed, which is
    // why the registry holds factories rather than instances.
    const TestCasePtr first = scoped.get().create("suite.name");
    const TestCasePtr second = scoped.get().create("suite.name");
    EXPECT_NE(first.get(), second.get());
}

TEST(Registry, DuplicateRegistrationIsRejected) {
    const ScopedRegistry scoped;
    const TestMetadata metadata = makeMetadata("suite", "name");
    scoped.get().registerTest(metadata, trivialFactory(metadata));

    // Silently overwriting would make a test vanish from the run, which is the
    // worst possible failure mode for a test tool.
    EXPECT_THROW(scoped.get().registerTest(metadata, trivialFactory(metadata)), FrameworkError);
}

TEST(Registry, RegisterOrReplaceOverwrites) {
    const ScopedRegistry scoped;
    const TestMetadata metadata = makeMetadata("gen", "name");
    scoped.get().registerTest(metadata, trivialFactory(metadata));
    EXPECT_NO_THROW(scoped.get().registerOrReplace(metadata, trivialFactory(metadata)));
    EXPECT_EQ(scoped.get().size(), 1U);
}

TEST(Registry, RemoveSuite) {
    const ScopedRegistry scoped;
    for (const char* name : {"a", "b", "c"}) {
        const TestMetadata metadata = makeMetadata("gen", name);
        scoped.get().registerTest(metadata, trivialFactory(metadata));
    }
    const TestMetadata keeper = makeMetadata("keep", "x");
    scoped.get().registerTest(keeper, trivialFactory(keeper));

    EXPECT_EQ(scoped.get().removeSuite("gen"), 3U);
    EXPECT_EQ(scoped.get().size(), 1U);
    EXPECT_TRUE(scoped.get().contains("keep.x"));
}

TEST(Registry, ListingIsSortedAndDeterministic) {
    const ScopedRegistry scoped;
    for (const char* name : {"zebra", "apple", "mango"}) {
        const TestMetadata metadata = makeMetadata("suite", name);
        scoped.get().registerTest(metadata, trivialFactory(metadata));
    }

    const std::vector<TestMetadata> all = scoped.get().all();
    ASSERT_EQ(all.size(), 3U);
    EXPECT_EQ(all[0].name, "apple");
    EXPECT_EQ(all[1].name, "mango");
    EXPECT_EQ(all[2].name, "zebra");
}

TEST(Registry, SuitesAndTagsAreDeduplicated) {
    const ScopedRegistry scoped;
    const TestMetadata a = makeMetadata("api", "one", {"smoke", "fast"});
    const TestMetadata b = makeMetadata("api", "two", {"smoke"});
    const TestMetadata c = makeMetadata("gpu", "three", {"hardware"});
    scoped.get().registerTest(a, trivialFactory(a));
    scoped.get().registerTest(b, trivialFactory(b));
    scoped.get().registerTest(c, trivialFactory(c));

    EXPECT_EQ(scoped.get().suites(), (std::vector<std::string>{"api", "gpu"}));
    EXPECT_EQ(scoped.get().tags(), (std::vector<std::string>{"fast", "hardware", "smoke"}));
}

// ---------------------------------------------------------------------------
// Selector
// ---------------------------------------------------------------------------

namespace {

std::vector<TestMetadata> sampleCatalogue() {
    return {
        makeMetadata("api", "health", {"api", "smoke"}),
        makeMetadata("api", "users", {"api", "regression"}),
        makeMetadata("gpu", "driver", {"gpu", "hardware"}),
        makeMetadata("smoke", "fast", {"smoke"}),
        [] {
            TestMetadata disabled = makeMetadata("api", "disabled", {"api"});
            disabled.enabled = false;
            disabled.disabledReason = "under construction";
            return disabled;
        }(),
    };
}

std::vector<std::string> names(const std::vector<TestMetadata>& tests) {
    std::vector<std::string> out;
    out.reserve(tests.size());
    for (const TestMetadata& test : tests) {
        out.push_back(test.qualifiedName());
    }
    return out;
}

}  // namespace

TEST(Selector, EmptyFilterSelectsEverythingEnabled) {
    const std::vector<TestMetadata> selected =
        TestSelector::select(sampleCatalogue(), TestFilter{});
    EXPECT_EQ(selected.size(), 4U) << "the disabled test should be excluded";
}

TEST(Selector, DisabledTestsAreOptIn) {
    TestFilter filter;
    filter.includeDisabled = true;
    EXPECT_EQ(TestSelector::select(sampleCatalogue(), filter).size(), 5U);
}

TEST(Selector, SuiteFilterSupportsGlobs) {
    TestFilter filter;
    filter.suites = {"api"};
    EXPECT_EQ(TestSelector::select(sampleCatalogue(), filter).size(), 2U);

    filter.suites = {"*p*"};
    EXPECT_EQ(TestSelector::select(sampleCatalogue(), filter).size(), 3U);
}

TEST(Selector, NameFilterMatchesQualifiedOrBareName) {
    TestFilter filter;
    filter.names = {"api.health"};
    EXPECT_EQ(names(TestSelector::select(sampleCatalogue(), filter)),
              (std::vector<std::string>{"api.health"}));

    // Bare name works too, so a user does not have to remember the suite.
    filter.names = {"health"};
    EXPECT_EQ(TestSelector::select(sampleCatalogue(), filter).size(), 1U);
}

TEST(Selector, TagFilterIsAnyOf) {
    TestFilter filter;
    filter.tags = {"smoke"};
    EXPECT_EQ(TestSelector::select(sampleCatalogue(), filter).size(), 2U);
}

TEST(Selector, DifferentFieldsCombineWithAnd) {
    // --suite api --tag smoke means the intersection, not the union.
    TestFilter filter;
    filter.suites = {"api"};
    filter.tags = {"smoke"};
    EXPECT_EQ(names(TestSelector::select(sampleCatalogue(), filter)),
              (std::vector<std::string>{"api.health"}));
}

TEST(Selector, Exclusions) {
    TestFilter filter;
    filter.excludeTags = {"gpu"};
    EXPECT_EQ(TestSelector::select(sampleCatalogue(), filter).size(), 3U);

    filter = TestFilter{};
    filter.excludeSuites = {"api"};
    EXPECT_EQ(TestSelector::select(sampleCatalogue(), filter).size(), 2U);
}

TEST(Selector, ResultIsSortedForDeterminism) {
    const std::vector<TestMetadata> selected =
        TestSelector::select(sampleCatalogue(), TestFilter{});
    std::vector<std::string> observed = names(selected);
    std::vector<std::string> sorted = observed;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(observed, sorted);
}

TEST(Selector, ShuffleIsReproducibleFromItsSeed) {
    SelectionOptions options;
    options.shuffle = true;
    options.seed = 12345;

    const std::vector<std::string> first =
        names(TestSelector::select(sampleCatalogue(), TestFilter{}, options));
    const std::vector<std::string> second =
        names(TestSelector::select(sampleCatalogue(), TestFilter{}, options));

    // Reporting the seed is only useful if replaying it reproduces the order.
    EXPECT_EQ(first, second);
}

TEST(Selector, ShardingPartitionsWithoutOverlap) {
    std::vector<TestMetadata> catalogue;
    for (int i = 0; i < 40; ++i) {
        catalogue.push_back(makeMetadata("suite", "test" + std::to_string(i)));
    }

    std::vector<std::string> combined;
    for (int shard = 0; shard < 4; ++shard) {
        SelectionOptions options;
        options.shardCount = 4;
        options.shardIndex = shard;
        for (const std::string& name :
             names(TestSelector::select(catalogue, TestFilter{}, options))) {
            combined.push_back(name);
        }
    }

    std::sort(combined.begin(), combined.end());
    EXPECT_EQ(combined.size(), 40U) << "every test must land in exactly one shard";
    EXPECT_EQ(std::unique(combined.begin(), combined.end()) - combined.begin(), 40);
}

TEST(Selector, ShardMembershipIsStableAsTestsAreAdded) {
    // Sharding by a hash of the test id rather than by position means adding a
    // test does not reshuffle every other test between shards.
    std::vector<TestMetadata> before;
    for (int i = 0; i < 20; ++i) {
        before.push_back(makeMetadata("suite", "test" + std::to_string(i)));
    }
    std::vector<TestMetadata> after = before;
    after.push_back(makeMetadata("suite", "brand_new"));

    SelectionOptions options;
    options.shardCount = 3;
    options.shardIndex = 1;

    std::vector<std::string> shardBefore =
        names(TestSelector::select(before, TestFilter{}, options));
    std::vector<std::string> shardAfter = names(TestSelector::select(after, TestFilter{}, options));

    for (const std::string& name : shardBefore) {
        EXPECT_NE(std::find(shardAfter.begin(), shardAfter.end(), name), shardAfter.end())
            << name << " moved between shards when an unrelated test was added";
    }
}

TEST(Filter, DescriptionRoundTripsIntoSomethingReadable) {
    TestFilter filter;
    filter.suites = {"api"};
    filter.tags = {"smoke", "fast"};
    const std::string described = filter.describe();
    EXPECT_NE(described.find("--suite api"), std::string::npos);
    EXPECT_NE(described.find("--tag smoke"), std::string::npos);

    EXPECT_EQ(TestFilter{}.describe(), "<all tests>");
    EXPECT_TRUE(TestFilter{}.empty());
    EXPECT_FALSE(filter.empty());
}
