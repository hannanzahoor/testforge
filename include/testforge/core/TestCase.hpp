#pragma once

#include "testforge/core/Json.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace testforge {

class TestContext;

/// Everything about a test that is known before it runs.
///
/// Metadata is separate from the TestCase object so that `testforge list`,
/// filtering and the REST API can work with test *descriptions* without
/// constructing or running anything.
struct TestMetadata {
    std::string name;   ///< "health_check" — unique within its suite
    std::string suite;  ///< "api"
    std::string description;
    std::vector<std::string> tags;

    /// 0 means "inherit execution.default_timeout_ms".
    std::int64_t timeoutMs = 0;

    bool enabled = true;
    std::string disabledReason;
    std::string owner;

    /// Where the test is defined; shown in listings and failure reports.
    std::string sourceFile;
    int sourceLine = 0;

    [[nodiscard]] std::string qualifiedName() const {
        return suite.empty() ? name : suite + "." + name;
    }

    [[nodiscard]] bool hasTag(std::string_view tag) const;

    [[nodiscard]] json::Value toJson() const;
};

/// A runnable test.
///
/// Departures from the classic xUnit shape, both deliberate:
///
///   * execute() takes a TestContext. Tests need configuration, a logger, a
///     cancellation token and somewhere to record metadata; injecting them
///     beats reaching for globals and makes every test independently
///     constructible in a unit test of the framework itself.
///
///   * tearDown() is noexcept. A throwing teardown would mask the original
///     failure — the thing the engineer actually needs to see. Teardown
///     problems are logged, not propagated.
class TestCase {
 public:
    virtual ~TestCase() = default;

    TestCase(const TestCase&) = delete;
    TestCase& operator=(const TestCase&) = delete;
    TestCase(TestCase&&) = delete;
    TestCase& operator=(TestCase&&) = delete;

    /// Runs before execute(). A throw here yields Error, not Failed: the test
    /// never got a chance to check anything.
    virtual void setUp(TestContext& context) { (void)context; }

    /// The test body. Report failure by throwing (the ASSERT_* macros do).
    virtual void execute(TestContext& context) = 0;

    /// Always runs if setUp() succeeded, including after a failure or timeout.
    virtual void tearDown(TestContext& context) noexcept { (void)context; }

    [[nodiscard]] virtual const TestMetadata& metadata() const = 0;

    [[nodiscard]] std::string name() const { return metadata().name; }

    [[nodiscard]] std::string suite() const { return metadata().suite; }

    [[nodiscard]] std::string qualifiedName() const { return metadata().qualifiedName(); }

 protected:
    TestCase() = default;
};

using TestCasePtr = std::unique_ptr<TestCase>;

/// Factory stored in the registry.
///
/// Tests are constructed fresh for every execution rather than being kept as
/// long-lived objects: that guarantees a retry starts from clean state and
/// that two workers never share a test instance.
using TestFactory = std::function<TestCasePtr()>;

/// Adapts a plain function into a TestCase. This is what the TESTFORGE_TEST
/// macro produces, and it is the shortest path from "I want to check X" to a
/// registered, runnable test.
class FunctionTestCase final : public TestCase {
 public:
    using Body = std::function<void(TestContext&)>;

    FunctionTestCase(TestMetadata metadata, Body body)
        : metadata_(std::move(metadata)), body_(std::move(body)) {}

    void execute(TestContext& context) override { body_(context); }

    [[nodiscard]] const TestMetadata& metadata() const override { return metadata_; }

 private:
    TestMetadata metadata_;
    Body body_;
};

}  // namespace testforge
