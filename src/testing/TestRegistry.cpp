#include "testforge/testing/TestRegistry.hpp"

#include "testforge/core/Exceptions.hpp"
#include "testforge/core/TestContext.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace testforge {

TestRegistry& TestRegistry::instance() {
    static TestRegistry registry;
    return registry;
}

void TestRegistry::registerTest(TestMetadata metadata, TestFactory factory) {
    const std::string key = metadata.qualifiedName();
    const std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.find(key) != entries_.end()) {
        throw FrameworkError("duplicate test registration: " + key +
                             " (a test with this suite and name already exists)");
    }
    entries_.emplace(key, Entry{std::move(metadata), std::move(factory)});
}

void TestRegistry::registerOrReplace(TestMetadata metadata, TestFactory factory) {
    const std::string key = metadata.qualifiedName();
    const std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = Entry{std::move(metadata), std::move(factory)};
}

std::size_t TestRegistry::removeSuite(std::string_view suite) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.metadata.suite == suite) {
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

bool TestRegistry::contains(std::string_view qualifiedName) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(qualifiedName) != entries_.end();
}

std::optional<TestMetadata> TestRegistry::find(std::string_view qualifiedName) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(qualifiedName);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return found->second.metadata;
}

TestCasePtr TestRegistry::create(std::string_view qualifiedName) const {
    TestFactory factory;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(qualifiedName);
        if (found == entries_.end()) {
            return nullptr;
        }
        // Copy the factory out and release the lock before calling it: a
        // factory is user code and may take arbitrarily long, or itself touch
        // the registry.
        factory = found->second.factory;
    }
    return factory ? factory() : nullptr;
}

std::vector<TestMetadata> TestRegistry::all() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TestMetadata> out;
    out.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        out.push_back(entry.metadata);
    }
    return out;
}

std::vector<std::string> TestRegistry::suites() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::string> unique;
    for (const auto& [key, entry] : entries_) {
        unique.insert(entry.metadata.suite);
    }
    return {unique.begin(), unique.end()};
}

std::vector<std::string> TestRegistry::tags() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::string> unique;
    for (const auto& [key, entry] : entries_) {
        for (const std::string& tag : entry.metadata.tags) {
            unique.insert(tag);
        }
    }
    return {unique.begin(), unique.end()};
}

std::size_t TestRegistry::size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void TestRegistry::clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

// ---------------------------------------------------------------------------
// AutoRegistration
// ---------------------------------------------------------------------------

AutoRegistration::AutoRegistration(const char* suite,
                                   const char* name,
                                   const char* file,
                                   int line,
                                   const TestOptions& options,
                                   void (*body)(TestContext&)) {
    TestMetadata metadata;
    metadata.suite = suite != nullptr ? suite : "";
    metadata.name = name != nullptr ? name : "";
    metadata.description = options.description;
    metadata.tags = options.tags;
    metadata.timeoutMs = options.timeoutMs;
    metadata.enabled = options.enabled;
    metadata.disabledReason = options.disabledReason;
    metadata.owner = options.owner;
    metadata.sourceFile = file != nullptr ? file : "";
    metadata.sourceLine = line;

    // Every test implicitly carries its suite as a tag, so `--tag api` and
    // `--suite api` both work without the author having to remember to add it.
    if (!metadata.suite.empty() && !metadata.hasTag(metadata.suite)) {
        metadata.tags.push_back(metadata.suite);
    }

    TestMetadata copy = metadata;
    TestRegistry::instance().registerTest(std::move(metadata), [copy, body]() -> TestCasePtr {
        return std::make_unique<FunctionTestCase>(copy,
                                                  [body](TestContext& context) { body(context); });
    });
}

}  // namespace testforge
