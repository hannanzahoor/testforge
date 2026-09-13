#pragma once

#include "testforge/core/TestCase.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace testforge {

class TestRegistry;

/// Declarative description of "which tests should run".
///
/// Every field is a list, and a non-empty list means "must match at least one
/// entry". Different fields combine with AND: `--suite api --tag smoke` runs
/// smoke tests in the api suite, not their union. That is what people expect,
/// and it is the only combination that lets a filter narrow rather than widen.
struct TestFilter {
    /// Glob patterns matched against the suite name: "api", "gpu*".
    std::vector<std::string> suites;

    /// Glob patterns matched against the qualified name ("api.health_check")
    /// and, as a convenience, against the bare test name.
    std::vector<std::string> names;

    std::vector<std::string> tags;
    std::vector<std::string> excludeTags;
    std::vector<std::string> excludeSuites;

    /// Disabled tests are listed but not run unless this is set.
    bool includeDisabled = false;

    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] bool matches(const TestMetadata& metadata) const;

    /// Human-readable rendering, stored on the run record so a historical run
    /// can be explained later ("--suite smoke --tag api").
    [[nodiscard]] std::string describe() const;
};

/// How a selection is ordered and split.
///
/// Declared at namespace scope rather than nested inside TestSelector: a
/// nested class's default member initialisers are not parsed until the end of
/// the enclosing class, so `const Options& = {}` on a member of that same
/// class does not compile.
struct SelectionOptions {
    bool shuffle = false;
    std::uint64_t seed = 0;  ///< 0 = derive from the clock

    /// Split the selection across N shards and run only shard `index`.
    /// Sharding is by stable test id, so the same test always lands in the
    /// same shard regardless of how many other tests exist.
    int shardCount = 1;
    int shardIndex = 0;
};

/// Turns a filter into an ordered list of tests to execute.
///
/// Ordering matters more than it looks: a deterministic order makes runs
/// comparable and bisectable, while a *seeded* shuffle is what surfaces tests
/// that only pass because of the order they happen to run in.
class TestSelector {
 public:
    /// Selects from the given registry.
    static std::vector<TestMetadata> select(const TestRegistry& registry,
                                            const TestFilter& filter,
                                            const SelectionOptions& options = {});

    /// Selects from an explicit list — used by tests of the selector itself
    /// and by the REST API, which filters an already-materialised catalogue.
    static std::vector<TestMetadata> select(const std::vector<TestMetadata>& candidates,
                                            const TestFilter& filter,
                                            const SelectionOptions& options = {});

    /// The seed actually used by the last shuffle. Reported so a shuffled run
    /// can be reproduced exactly.
    [[nodiscard]] static std::uint64_t resolveSeed(std::uint64_t requested);
};

}  // namespace testforge
