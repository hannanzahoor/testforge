#include "testforge/ai/HttpAiProvider.hpp"

#include "testforge/core/Logger.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/net/Url.hpp"

#include <algorithm>
#include <utility>

namespace testforge::ai {
namespace {

Logger& logger() {
    static Logger instance("ai.http");
    return instance;
}

std::vector<std::string> readStringArray(const json::Value& object, std::string_view key) {
    std::vector<std::string> out;
    const json::Value* found = object.find(key);
    if (found == nullptr || !found->isArray()) {
        return out;
    }
    for (const json::Value& item : found->asArray()) {
        if (item.isString()) {
            // Model output goes into reports and terminals; bound it and strip
            // anything that looks like a credential it may have echoed back.
            out.push_back(strings::truncate(strings::redactSecrets(item.asString()), 500));
        }
    }
    return out;
}

std::string readString(const json::Value& object, std::string_view key) {
    const json::Value* found = object.find(key);
    return (found != nullptr && found->isString())
               ? strings::truncate(strings::redactSecrets(found->asString()), 2000)
               : std::string{};
}

}  // namespace

HttpAiProvider::HttpAiProvider(AiConfig config) : config_(std::move(config)) {
    net::HttpClientOptions options;
    options.requestTimeout = Milliseconds{config_.timeoutMs};
    options.connectTimeout = Milliseconds{std::min<std::int64_t>(config_.timeoutMs, 5'000)};
    options.maxResponseBytes = static_cast<std::size_t>(config_.maxResponseBytes);
    options.followRedirects = false;  // the sidecar never redirects
    options.userAgent = "TestForge/0.9 (ai-client)";
    // The sidecar is a local process; the policy pins the client to its host so
    // a mistyped endpoint cannot turn into a request to somewhere unexpected.
    options.policy = net::UrlPolicy::restrictedTo(config_.endpoint);
    client_ = std::make_unique<net::HttpClient>(std::move(options));
}

HttpAiProvider::~HttpAiProvider() = default;

bool HttpAiProvider::isEnabled() const {
    return config_.enabled && !config_.endpoint.empty();
}

std::string HttpAiProvider::endpoint(std::string_view path) const {
    const std::optional<std::string> joined = net::joinUrl(config_.endpoint, path);
    return joined.value_or(config_.endpoint);
}

GenerationResult HttpAiProvider::generateTests(const GenerationRequest& request) {
    GenerationResult result;
    result.provider = "http-sidecar";
    result.model = config_.model;

    if (!isEnabled()) {
        result.error = "the AI provider is not enabled";
        return result;
    }

    json::Value payload = json::Value::object();
    // The requirement is user text. It is sent as a *data field*, never
    // concatenated into a prompt here — the sidecar is responsible for keeping
    // it inside a user-role message. See docs/security.md on prompt injection.
    payload.set("requirement", request.requirement);
    payload.set("suite", request.suiteName);
    payload.set("base_url", request.baseUrl);
    payload.set("max_tests", std::min(request.maxTests, config_.maxGeneratedTests));
    payload.set("model", config_.model);
    payload.set("context", request.context);

    const Stopwatch watch;
    const net::HttpResponse response = client_->postJson(endpoint("/generate-tests"), payload);
    result.elapsed = watch.elapsed();

    if (response.transportError) {
        result.error =
            "cannot reach the AI sidecar at " + config_.endpoint + ": " + response.errorMessage;
        logger().warn("AI sidecar unreachable",
                      {{"endpoint", config_.endpoint}, {"error", response.errorMessage}});
        return result;
    }
    if (!response.ok()) {
        result.error = "the AI sidecar returned HTTP " + std::to_string(response.statusCode) +
                       ": " + strings::truncate(response.body, 500);
        return result;
    }

    std::string parseError;
    const std::optional<json::Value> document = json::tryParse(response.body, &parseError);
    if (!document.has_value()) {
        result.error = "the AI sidecar returned a body that is not valid JSON: " + parseError;
        return result;
    }
    result.rawResponse = *document;

    if (const json::Value* ok = document->find("ok"); ok != nullptr && !ok->boolOr(true)) {
        result.error = readString(*document, "error");
        if (result.error.empty()) {
            result.error = "the AI sidecar reported a failure without a message";
        }
        return result;
    }

    if (const json::Value* usage = document->find("usage"); usage != nullptr) {
        result.promptTokens =
            usage->find("prompt_tokens") != nullptr ? usage->find("prompt_tokens")->intOr(0) : 0;
        result.completionTokens = usage->find("completion_tokens") != nullptr
                                      ? usage->find("completion_tokens")->intOr(0)
                                      : 0;
    }
    if (const json::Value* model = document->find("model"); model != nullptr) {
        result.model = model->stringOr(config_.model);
    }

    const json::Value* specification = document->find("specification");
    if (specification == nullptr) {
        result.error = "the AI response has no 'specification' object";
        return result;
    }

    std::string specError;
    std::optional<TestSpecSuite> suite = TestSpecSuite::fromJson(*specification, &specError);
    if (!suite.has_value()) {
        // Structured-output failures are common enough to be worth reporting
        // precisely: it is the difference between "the model is wrong" and
        // "TestForge is wrong".
        result.error = "the AI response did not match the required schema: " + specError;
        return result;
    }

    suite->source = "ai:" + result.model;
    suite->model = result.model;
    if (suite->requirement.empty()) {
        suite->requirement = request.requirement;
    }
    result.suite = std::move(*suite);
    result.ok = true;
    return result;
}

AnalysisResult HttpAiProvider::analyseFailure(const AnalysisRequest& request) {
    AnalysisResult result;
    result.provider = "http-sidecar";
    result.model = config_.model;

    if (!isEnabled()) {
        result.error = "the AI provider is not enabled";
        return result;
    }

    json::Value payload = json::Value::object();
    payload.set("context", request.context);
    payload.set("test", request.testName);
    payload.set("failure_category", request.failureCategory);
    payload.set("model", config_.model);

    const Stopwatch watch;
    const net::HttpResponse response = client_->postJson(endpoint("/analyze-failure"), payload);
    result.elapsed = watch.elapsed();

    if (response.transportError) {
        result.error =
            "cannot reach the AI sidecar at " + config_.endpoint + ": " + response.errorMessage;
        return result;
    }
    if (!response.ok()) {
        result.error = "the AI sidecar returned HTTP " + std::to_string(response.statusCode) +
                       ": " + strings::truncate(response.body, 500);
        return result;
    }

    std::string parseError;
    const std::optional<json::Value> document = json::tryParse(response.body, &parseError);
    if (!document.has_value()) {
        result.error = "the AI sidecar returned a body that is not valid JSON: " + parseError;
        return result;
    }
    if (const json::Value* ok = document->find("ok"); ok != nullptr && !ok->boolOr(true)) {
        result.error = readString(*document, "error");
        return result;
    }

    const json::Value* analysis = document->find("analysis");
    if (analysis == nullptr || !analysis->isObject()) {
        result.error = "the AI response has no 'analysis' object";
        return result;
    }

    result.probableCause = readString(*analysis, "probable_cause");
    result.suggestedCategory = readString(*analysis, "category");
    result.evidence = readStringArray(*analysis, "evidence");
    result.suggestedInvestigation = readStringArray(*analysis, "suggested_investigation");
    if (const json::Value* confidence = analysis->find("confidence");
        confidence != nullptr && confidence->isNumber()) {
        result.confidence = std::clamp(confidence->doubleOr(0.0), 0.0, 1.0);
    }
    if (const json::Value* model = document->find("model"); model != nullptr) {
        result.model = model->stringOr(config_.model);
    }

    if (result.probableCause.empty()) {
        result.error = "the AI response contained no probable cause";
        return result;
    }

    result.ok = true;
    return result;
}

json::Value HttpAiProvider::health() {
    json::Value out = json::Value::object();
    out.set("provider", "http-sidecar");
    out.set("enabled", isEnabled());
    out.set("endpoint", config_.endpoint);
    out.set("model", config_.model);
    out.set("timeout_ms", config_.timeoutMs);

    if (!isEnabled()) {
        out.set("reachable", false);
        out.set("reason", "AI features are disabled in the configuration");
        return out;
    }

    const net::HttpResponse response = client_->get(endpoint("/health"));
    out.set("reachable", !response.transportError && response.ok());
    out.set("status_code", response.statusCode);
    if (response.transportError) {
        out.set("error", response.errorMessage);
        out.set("hint",
                "Start the sidecar with: uvicorn python.ai.service:app --port 8810 "
                "(and export OPENAI_API_KEY for live model calls)");
    } else if (const std::optional<json::Value> body = response.json(); body.has_value()) {
        out.set("sidecar", *body);
    }
    return out;
}

}  // namespace testforge::ai
