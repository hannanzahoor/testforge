#include "testforge/core/TestContext.hpp"

#include "testforge/core/Exceptions.hpp"

#include <algorithm>
#include <utility>

namespace testforge {

TestContext::TestContext(const Config& config,
                         const TestMetadata& metadata,
                         CancellationToken cancellation,
                         std::string runId,
                         std::string worker)
    : config_(&config),
      metadata_(&metadata),
      cancellation_(std::move(cancellation)),
      runId_(std::move(runId)),
      worker_(std::move(worker)),
      logger_(Logger("test").forTest(metadata.qualifiedName())) {}

void TestContext::throwIfCancelled() const {
    if (cancellation_.isCancelled()) {
        std::string reason = cancellation_.reason();
        if (reason.empty()) {
            reason = "cancelled";
        }
        throw TestCancelled(reason);
    }
}

void TestContext::addMetadata(std::string key, json::Value value) {
    metadata_json_.set(std::move(key), std::move(value));
}

void TestContext::addTag(std::string tag) {
    if (std::find(tags_.begin(), tags_.end(), tag) == tags_.end()) {
        tags_.push_back(std::move(tag));
    }
}

void TestContext::addNote(std::string note) {
    notes_.push_back(std::move(note));
}

void TestContext::skip(std::string reason) const {
    throw SkipTest(std::move(reason));
}

const json::Value* TestContext::configValue(std::string_view dottedPath) const {
    return config_->custom.path(dottedPath);
}

Milliseconds TestContext::effectiveTimeout() const {
    const std::int64_t fromMetadata = metadata_->timeoutMs;
    if (fromMetadata > 0) {
        return Milliseconds{fromMetadata};
    }
    return Milliseconds{config_->execution.defaultTimeoutMs};
}

}  // namespace testforge

// TestMetadata helpers live here rather than in a file of their own.
namespace testforge {

bool TestMetadata::hasTag(std::string_view tag) const {
    return std::any_of(
        tags.begin(), tags.end(), [tag](const std::string& candidate) { return candidate == tag; });
}

json::Value TestMetadata::toJson() const {
    json::Value out = json::Value::object();
    out.set("name", name);
    out.set("suite", suite);
    out.set("qualified_name", qualifiedName());
    out.set("description", description);
    json::Value tagList = json::Value::array();
    for (const std::string& tag : tags) {
        tagList.push(tag);
    }
    out.set("tags", tagList);
    out.set("timeout_ms", timeoutMs);
    out.set("enabled", enabled);
    if (!disabledReason.empty()) {
        out.set("disabled_reason", disabledReason);
    }
    if (!owner.empty()) {
        out.set("owner", owner);
    }
    if (!sourceFile.empty()) {
        out.set("source_file", sourceFile);
        out.set("source_line", sourceLine);
    }
    return out;
}

}  // namespace testforge
