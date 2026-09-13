#include "testforge/testing/TestSelector.hpp"

#include "testforge/core/Ids.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/testing/TestRegistry.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>

namespace testforge {
namespace {

bool matchesAnyGlob(const std::vector<std::string>& patterns, std::string_view text) {
    return std::any_of(patterns.begin(), patterns.end(), [text](const std::string& pattern) {
        return strings::globMatch(pattern, text);
    });
}

void appendFilterPart(std::ostringstream& os,
                      std::string_view flag,
                      const std::vector<std::string>& values) {
    for (const std::string& value : values) {
        if (os.tellp() > 0) {
            os << ' ';
        }
        os << flag << ' ' << value;
    }
}

}  // namespace

bool TestFilter::empty() const noexcept {
    return suites.empty() && names.empty() && tags.empty() && excludeTags.empty() &&
           excludeSuites.empty();
}

bool TestFilter::matches(const TestMetadata& metadata) const {
    if (!metadata.enabled && !includeDisabled) {
        return false;
    }

    if (!suites.empty() && !matchesAnyGlob(suites, metadata.suite)) {
        return false;
    }
    if (!excludeSuites.empty() && matchesAnyGlob(excludeSuites, metadata.suite)) {
        return false;
    }

    if (!names.empty()) {
        const std::string qualified = metadata.qualifiedName();
        // Match the qualified name first; falling back to the bare name means
        // `--test health_check` works without having to know the suite.
        if (!matchesAnyGlob(names, qualified) && !matchesAnyGlob(names, metadata.name)) {
            return false;
        }
    }

    if (!tags.empty()) {
        const bool anyTagMatches =
            std::any_of(metadata.tags.begin(), metadata.tags.end(), [this](const std::string& tag) {
                return matchesAnyGlob(tags, tag);
            });
        if (!anyTagMatches) {
            return false;
        }
    }

    if (!excludeTags.empty()) {
        const bool anyExcluded =
            std::any_of(metadata.tags.begin(), metadata.tags.end(), [this](const std::string& tag) {
                return matchesAnyGlob(excludeTags, tag);
            });
        if (anyExcluded) {
            return false;
        }
    }

    return true;
}

std::string TestFilter::describe() const {
    std::ostringstream os;
    appendFilterPart(os, "--suite", suites);
    appendFilterPart(os, "--test", names);
    appendFilterPart(os, "--tag", tags);
    appendFilterPart(os, "--exclude-tag", excludeTags);
    appendFilterPart(os, "--exclude-suite", excludeSuites);
    if (includeDisabled) {
        if (os.tellp() > 0) {
            os << ' ';
        }
        os << "--include-disabled";
    }
    const std::string text = os.str();
    return text.empty() ? "<all tests>" : text;
}

std::uint64_t TestSelector::resolveSeed(std::uint64_t requested) {
    if (requested != 0) {
        return requested;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::vector<TestMetadata> TestSelector::select(const std::vector<TestMetadata>& candidates,
                                               const TestFilter& filter,
                                               const SelectionOptions& options) {
    std::vector<TestMetadata> selected;
    selected.reserve(candidates.size());
    for (const TestMetadata& metadata : candidates) {
        if (filter.matches(metadata)) {
            selected.push_back(metadata);
        }
    }

    // Sort before sharding or shuffling so the input order is always the same
    // regardless of how the candidates were produced.
    std::sort(selected.begin(), selected.end(), [](const TestMetadata& a, const TestMetadata& b) {
        return a.qualifiedName() < b.qualifiedName();
    });

    if (options.shardCount > 1) {
        const auto shardCount = static_cast<std::uint64_t>(options.shardCount);
        const auto shardIndex = static_cast<std::uint64_t>(std::max(0, options.shardIndex));
        std::vector<TestMetadata> shard;
        for (const TestMetadata& metadata : selected) {
            // Hash of the stable id, not the position, so adding a test does
            // not reshuffle every other test between shards.
            const std::uint64_t hash = ids::fnv1a64(metadata.qualifiedName());
            if (hash % shardCount == shardIndex % shardCount) {
                shard.push_back(metadata);
            }
        }
        selected = std::move(shard);
    }

    if (options.shuffle) {
        std::mt19937_64 engine(resolveSeed(options.seed));
        std::shuffle(selected.begin(), selected.end(), engine);
    }

    return selected;
}

std::vector<TestMetadata> TestSelector::select(const TestRegistry& registry,
                                               const TestFilter& filter,
                                               const SelectionOptions& options) {
    return select(registry.all(), filter, options);
}

}  // namespace testforge
