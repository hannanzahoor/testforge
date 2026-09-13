#include "testforge/ai/AiProvider.hpp"

#include "testforge/ai/HttpAiProvider.hpp"
#include "testforge/ai/MockAiProvider.hpp"
#include "testforge/core/Environment.hpp"

#include <sstream>
#include <utility>

namespace testforge::ai {
namespace {

/// Reports "disabled" for every operation, with the reason attached.
///
/// A null-object rather than a nullptr: every call site would otherwise need
/// its own null check, and the one that forgot would crash on a machine with
/// no API key — the exact configuration this project must handle gracefully.
class DisabledAiProvider final : public AiProvider {
 public:
    explicit DisabledAiProvider(std::string reason) : reason_(std::move(reason)) {}

    [[nodiscard]] std::string_view name() const override { return "disabled"; }

    [[nodiscard]] bool isEnabled() const override { return false; }

    [[nodiscard]] GenerationResult generateTests(const GenerationRequest& /*request*/) override {
        GenerationResult result;
        result.ok = false;
        result.provider = "disabled";
        result.error = reason_;
        return result;
    }

    [[nodiscard]] AnalysisResult analyseFailure(const AnalysisRequest& /*request*/) override {
        AnalysisResult result;
        result.ok = false;
        result.provider = "disabled";
        result.error = reason_;
        return result;
    }

    [[nodiscard]] json::Value health() override {
        json::Value out = json::Value::object();
        out.set("provider", "disabled");
        out.set("enabled", false);
        out.set("reason", reason_);
        out.set("hint",
                "Set ai.enabled=true (or TESTFORGE_AI_ENABLED=1) and start the Python AI "
                "sidecar: python -m uvicorn python.ai.service:app --port 8810");
        return out;
    }

 private:
    std::string reason_;
};

}  // namespace

json::Value GenerationResult::toJson() const {
    json::Value out = json::Value::object();
    out.set("ok", ok);
    if (!ok) {
        out.set("error", error);
    }
    out.set("provider", provider);
    out.set("model", model);
    out.set("elapsed_ms", millisOf(elapsed));
    out.set("prompt_tokens", promptTokens);
    out.set("completion_tokens", completionTokens);
    out.set("suite", suite.toJson());
    return out;
}

json::Value AnalysisResult::toJson() const {
    json::Value out = json::Value::object();
    out.set("ok", ok);
    if (!ok) {
        out.set("error", error);
    }
    out.set("provider", provider);
    out.set("model", model);
    out.set("elapsed_ms", millisOf(elapsed));
    out.set("probable_cause", probableCause);
    out.set("suggested_category", suggestedCategory);
    out.set("confidence", confidence);

    json::Value evidenceList = json::Value::array();
    for (const std::string& item : evidence) {
        evidenceList.push(item);
    }
    out.set("evidence", evidenceList);

    json::Value steps = json::Value::array();
    for (const std::string& item : suggestedInvestigation) {
        steps.push(item);
    }
    out.set("suggested_investigation", steps);

    // Emitted on every response so no consumer can present this as a verdict.
    out.set("advisory_only", true);
    out.set("disclaimer",
            "AI analysis is advisory. The pass/fail verdict was decided by the assertions in "
            "the C++ engine and is not affected by anything in this object.");
    return out;
}

std::string AnalysisResult::render() const {
    std::ostringstream os;
    if (!ok) {
        os << "AI analysis unavailable: " << error << '\n';
        return os.str();
    }

    os << "AI analysis (advisory — the verdict above was decided by the test engine)\n";
    os << "  probable cause    " << probableCause << '\n';
    if (!suggestedCategory.empty()) {
        os << "  model's category  " << suggestedCategory << "  (not applied to the result)\n";
    }
    if (confidence > 0.0) {
        std::ostringstream percent;
        percent.setf(std::ios::fixed);
        percent.precision(0);
        percent << confidence * 100.0 << '%';
        os << "  self-reported confidence " << percent.str() << '\n';
    }
    if (!evidence.empty()) {
        os << "  evidence\n";
        for (const std::string& item : evidence) {
            os << "    - " << item << '\n';
        }
    }
    if (!suggestedInvestigation.empty()) {
        os << "  suggested investigation\n";
        int step = 1;
        for (const std::string& item : suggestedInvestigation) {
            os << "    " << step++ << ". " << item << '\n';
        }
    }
    if (!model.empty()) {
        os << "  model             " << model << " via " << provider << '\n';
    }
    return os.str();
}

AiProviderPtr makeDisabledProvider(std::string reason) {
    return std::make_shared<DisabledAiProvider>(std::move(reason));
}

AiProviderPtr createProvider(const AiConfig& config) {
    if (!config.enabled) {
        return makeDisabledProvider(
            "AI features are disabled (set ai.enabled=true or TESTFORGE_AI_ENABLED=1)");
    }
    // An explicit opt-in, never a fallback: if the sidecar is unreachable the
    // right answer is an error, not silently synthesised "AI" output.
    if (env::getBool("TESTFORGE_AI_MOCK", false)) {
        return std::make_shared<MockAiProvider>();
    }
    return std::make_shared<HttpAiProvider>(config);
}

}  // namespace testforge::ai
