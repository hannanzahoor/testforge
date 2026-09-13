#pragma once

#include "testforge/ai/AiProvider.hpp"

#include <string>

namespace testforge::ai {

/// A deterministic stand-in for a language model.
///
/// Two jobs, both of which need output that is identical on every run:
///   * unit tests of the AI plumbing — validation, spec execution, report
///     rendering — without a network call or an API key;
///   * the offline demo, so `testforge ai generate-tests --mock` shows the
///     whole pipeline on a machine with no credentials.
///
/// It does not pretend to be intelligent. It applies a handful of rules to the
/// requirement text (mentions of "create" produce a POST case, "invalid"
/// produces a 400 case) and labels everything it emits as mock output, so a
/// reader can never mistake it for a real generation.
class MockAiProvider final : public AiProvider {
 public:
    MockAiProvider() = default;

    [[nodiscard]] std::string_view name() const override { return "mock"; }

    [[nodiscard]] bool isEnabled() const override { return true; }

    [[nodiscard]] GenerationResult generateTests(const GenerationRequest& request) override;

    [[nodiscard]] AnalysisResult analyseFailure(const AnalysisRequest& request) override;

    [[nodiscard]] json::Value health() override;

    /// Makes every call fail, for exercising error handling.
    void setFailureMode(std::string error) { forcedError_ = std::move(error); }

 private:
    std::string forcedError_;
};

}  // namespace testforge::ai
