#pragma once

#include "testforge/ai/AiProvider.hpp"
#include "testforge/net/HttpClient.hpp"

#include <memory>
#include <string>

namespace testforge::ai {

/// Talks to the TestForge AI sidecar over HTTP.
///
/// Why a sidecar instead of calling api.openai.com from C++:
///
///   1. Secrets. The API key lives in one process, written in the language
///      whose SDK owns the retry, streaming and rate-limit behaviour. The C++
///      engine never sees a credential.
///   2. TLS. TestForge deliberately links no TLS stack (see HttpClient), so it
///      cannot reach an https endpoint. The sidecar can.
///   3. Iteration. Prompts and model choices change far more often than the
///      test engine does; keeping them in Python means changing a prompt does
///      not mean recompiling.
///
/// The sidecar exposes /health, /generate-tests and /analyze-failure and
/// answers with the JSON contract in docs/ai-architecture.md. This class
/// treats every response as untrusted: it is size-limited, parsed defensively,
/// and passed to SpecValidator before anything can run.
class HttpAiProvider final : public AiProvider {
 public:
    explicit HttpAiProvider(AiConfig config);

    ~HttpAiProvider() override;

    [[nodiscard]] std::string_view name() const override { return "http-sidecar"; }

    [[nodiscard]] bool isEnabled() const override;

    [[nodiscard]] GenerationResult generateTests(const GenerationRequest& request) override;

    [[nodiscard]] AnalysisResult analyseFailure(const AnalysisRequest& request) override;

    [[nodiscard]] json::Value health() override;

 private:
    [[nodiscard]] std::string endpoint(std::string_view path) const;

    AiConfig config_;
    std::unique_ptr<net::HttpClient> client_;
};

}  // namespace testforge::ai
