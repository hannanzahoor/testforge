#pragma once

#include "testforge/ai/TestSpec.hpp"
#include "testforge/core/Clock.hpp"
#include "testforge/core/Config.hpp"
#include "testforge/core/Json.hpp"
#include "testforge/core/TestResult.hpp"

#include <memory>
#include <string>
#include <vector>

namespace testforge::ai {

struct GenerationRequest {
    std::string requirement;  ///< natural language, from the user
    std::string suiteName = "generated";
    std::string baseUrl;
    int maxTests = 10;

    /// Optional extra context: known endpoints, an OpenAPI excerpt.
    json::Value context = json::Value::object();
};

struct GenerationResult {
    bool ok = false;
    std::string error;

    TestSpecSuite suite;

    /// Exactly what came back, before validation. Kept so a rejected
    /// generation can be inspected rather than guessed at.
    json::Value rawResponse = json::Value::object();

    std::string model;
    std::string provider;
    Milliseconds elapsed{0};
    std::int64_t promptTokens = 0;
    std::int64_t completionTokens = 0;

    [[nodiscard]] json::Value toJson() const;
};

/// Structured failure evidence handed to the model. Built by FailureContext.
struct AnalysisRequest {
    json::Value context = json::Value::object();
    std::string testName;
    std::string failureCategory;
};

/// The model's opinion about a failure.
///
/// Every field is advisory. Nothing here can change a TestResult: the verdict
/// was decided by the assertions before this call was made, and is not
/// revisited. See docs/ai-architecture.md.
struct AnalysisResult {
    bool ok = false;
    std::string error;

    std::string probableCause;
    std::string suggestedCategory;  ///< the model's guess, recorded but not applied
    std::vector<std::string> evidence;
    std::vector<std::string> suggestedInvestigation;

    /// 0.0-1.0, self-reported by the model. Displayed with that caveat.
    double confidence = 0.0;

    std::string model;
    std::string provider;
    Milliseconds elapsed{0};

    [[nodiscard]] json::Value toJson() const;

    /// Human-readable block for the console and reports, including the
    /// advisory disclaimer.
    [[nodiscard]] std::string render() const;
};

/// Boundary between TestForge and any language model.
///
/// The interface is deliberately tiny — generate, analyse, health — and
/// returns structured types rather than text. That means the rest of the
/// codebase never handles a model's prose, and swapping the provider (a
/// different vendor, a local model, a recorded fixture) changes one class.
class AiProvider {
 public:
    virtual ~AiProvider() = default;

    AiProvider(const AiProvider&) = delete;
    AiProvider& operator=(const AiProvider&) = delete;
    AiProvider(AiProvider&&) = delete;
    AiProvider& operator=(AiProvider&&) = delete;

    [[nodiscard]] virtual std::string_view name() const = 0;

    /// False when AI features are switched off or unconfigured. Callers check
    /// this and degrade gracefully instead of surfacing an error.
    [[nodiscard]] virtual bool isEnabled() const = 0;

    [[nodiscard]] virtual GenerationResult generateTests(const GenerationRequest& request) = 0;

    [[nodiscard]] virtual AnalysisResult analyseFailure(const AnalysisRequest& request) = 0;

    /// Liveness and configuration report, for `testforge ai status`.
    [[nodiscard]] virtual json::Value health() = 0;

 protected:
    AiProvider() = default;
};

using AiProviderPtr = std::shared_ptr<AiProvider>;

/// Returns a provider that reports "disabled" for everything, with `reason`
/// explaining why. Used when ai.enabled is false, so call sites never need a
/// null check.
AiProviderPtr makeDisabledProvider(std::string reason);

/// Chooses a provider from configuration:
///   * ai.enabled == false          -> disabled provider
///   * TESTFORGE_AI_MOCK=1          -> MockAiProvider (offline demo and tests)
///   * otherwise                    -> HttpAiProvider pointed at the sidecar
AiProviderPtr createProvider(const AiConfig& config);

}  // namespace testforge::ai
