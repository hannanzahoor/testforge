#pragma once

#include "testforge/core/TestCase.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace testforge {

/// Chainable metadata for a test declaration. Exists so the registration
/// macros can stay a single line while still allowing tags, timeouts and
/// ownership to be attached.
struct TestOptions {
    std::string description;
    std::vector<std::string> tags;
    std::int64_t timeoutMs = 0;
    std::string owner;
    bool enabled = true;
    std::string disabledReason;

    TestOptions& describe(std::string text) {
        description = std::move(text);
        return *this;
    }

    TestOptions& withTags(std::vector<std::string> values) {
        tags = std::move(values);
        return *this;
    }

    TestOptions& withTag(std::string value) {
        tags.push_back(std::move(value));
        return *this;
    }

    TestOptions& withTimeout(std::int64_t milliseconds) {
        timeoutMs = milliseconds;
        return *this;
    }

    TestOptions& withOwner(std::string value) {
        owner = std::move(value);
        return *this;
    }

    TestOptions& disable(std::string reason) {
        enabled = false;
        disabledReason = std::move(reason);
        return *this;
    }
};

/// The catalogue of every test known to this process.
///
/// Holding factories rather than instances is deliberate: a test object is
/// built immediately before it runs and destroyed immediately after, so a
/// retry cannot inherit state from the attempt that failed, and two workers
/// can never touch the same instance.
///
/// The registry is a singleton because static registration at namespace scope
/// has nowhere else to put its results. It is fully thread-safe, which matters
/// because the REST API registers AI-generated tests at run time while a run
/// may be in progress.
class TestRegistry {
 public:
    static TestRegistry& instance();

    /// Registers a test. Throws FrameworkError on a duplicate qualified name —
    /// silently overwriting would mean a test simply disappears from the run,
    /// which is the worst possible failure mode for a test tool.
    void registerTest(TestMetadata metadata, TestFactory factory);

    /// Registers, replacing any existing test with the same name. Used for
    /// dynamically generated suites that are regenerated on each request.
    void registerOrReplace(TestMetadata metadata, TestFactory factory);

    /// Removes every test in a suite. Returns how many were removed.
    std::size_t removeSuite(std::string_view suite);

    [[nodiscard]] bool contains(std::string_view qualifiedName) const;

    [[nodiscard]] std::optional<TestMetadata> find(std::string_view qualifiedName) const;

    /// Instantiates a test. Returns nullptr when the name is unknown.
    [[nodiscard]] TestCasePtr create(std::string_view qualifiedName) const;

    /// All registered tests, sorted by qualified name so that listings and
    /// unfiltered runs are deterministic.
    [[nodiscard]] std::vector<TestMetadata> all() const;

    [[nodiscard]] std::vector<std::string> suites() const;

    /// Every distinct tag, sorted.
    [[nodiscard]] std::vector<std::string> tags() const;

    [[nodiscard]] std::size_t size() const;

    /// Drops every registration. Only used by TestForge's own unit tests.
    void clear();

 private:
    TestRegistry() = default;

    struct Entry {
        TestMetadata metadata;
        TestFactory factory;
    };

    mutable std::mutex mutex_;
    // std::map keeps entries ordered by qualified name, which gives
    // deterministic listing order for free.
    std::map<std::string, Entry, std::less<>> entries_;
};

/// Registration helper constructed at namespace scope by the macros below.
///
/// Static-initialisation-order is not a hazard here: TestRegistry::instance()
/// is a function-local static, so it is constructed on first use — that is,
/// by the first AutoRegistration that runs.
class AutoRegistration {
 public:
    AutoRegistration(const char* suite,
                     const char* name,
                     const char* file,
                     int line,
                     const TestOptions& options,
                     void (*body)(TestContext&));
};

}  // namespace testforge

// ---------------------------------------------------------------------------
// Declaration macros
// ---------------------------------------------------------------------------

#define TESTFORGE_DETAIL_CONCAT_INNER(a, b) a##b
#define TESTFORGE_DETAIL_CONCAT(a, b) TESTFORGE_DETAIL_CONCAT_INNER(a, b)

/// Declares and registers a test with explicit options.
///
///     TESTFORGE_TEST_OPTS(api, health_check,
///                         ::testforge::TestOptions{}.withTags({"smoke"})) {
///         ctx.log().info("checking health");
///     }
///
/// The body receives `ctx`, a TestContext&.
#define TESTFORGE_TEST_OPTS(suiteId, testId, options)                                          \
    static void TESTFORGE_DETAIL_CONCAT(                                                       \
        tfBody_, TESTFORGE_DETAIL_CONCAT(suiteId, _##testId))(::testforge::TestContext&);      \
    static const ::testforge::AutoRegistration TESTFORGE_DETAIL_CONCAT(                        \
        tfReg_, TESTFORGE_DETAIL_CONCAT(suiteId, _##testId))(                                  \
        #suiteId,                                                                              \
        #testId,                                                                               \
        __FILE__,                                                                              \
        __LINE__,                                                                              \
        (options),                                                                             \
        &TESTFORGE_DETAIL_CONCAT(tfBody_, TESTFORGE_DETAIL_CONCAT(suiteId, _##testId)));       \
    static void TESTFORGE_DETAIL_CONCAT(tfBody_, TESTFORGE_DETAIL_CONCAT(suiteId, _##testId))( \
        [[maybe_unused]] ::testforge::TestContext & ctx)

/// Declares and registers a test with default options.
#define TESTFORGE_TEST(suiteId, testId) \
    TESTFORGE_TEST_OPTS(suiteId, testId, ::testforge::TestOptions{})

/// Declares a test carrying tags, the common case.
#define TESTFORGE_TEST_TAGGED(suiteId, testId, ...) \
    TESTFORGE_TEST_OPTS(suiteId, testId, ::testforge::TestOptions{}.withTags({__VA_ARGS__}))
