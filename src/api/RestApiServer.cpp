#include "testforge/api/RestApiServer.hpp"

#include "testforge/core/Logger.hpp"
#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <utility>

namespace testforge::api {
namespace {

using net::ServerRequest;
using net::ServerResponse;

Logger& logger() {
    static Logger instance("api");
    return instance;
}

/// Builds a filter from query parameters. Repeated values are comma-separated:
/// ?tag=api,smoke.
TestFilter filterFromQuery(const ServerRequest& request) {
    TestFilter filter;
    const auto collect = [&request](std::string_view name, std::vector<std::string>& into) {
        const std::string raw = request.queryParameter(name);
        if (raw.empty()) {
            return;
        }
        for (std::string& value : strings::split(raw, ',', true)) {
            into.push_back(strings::trim(value));
        }
    };
    collect("suite", filter.suites);
    collect("test", filter.names);
    collect("tag", filter.tags);
    collect("exclude_tag", filter.excludeTags);
    collect("exclude_suite", filter.excludeSuites);
    return filter;
}

/// Same, from a JSON request body.
TestFilter filterFromBody(const json::Value& body) {
    TestFilter filter;
    const auto collect = [&body](std::string_view key, std::vector<std::string>& into) {
        const json::Value* value = body.find(key);
        if (value == nullptr) {
            return;
        }
        if (value->isString()) {
            for (std::string& item : strings::split(value->asString(), ',', true)) {
                into.push_back(strings::trim(item));
            }
        } else if (value->isArray()) {
            for (const json::Value& item : value->asArray()) {
                if (item.isString()) {
                    into.push_back(item.asString());
                }
            }
        }
    };
    collect("suite", filter.suites);
    collect("test", filter.names);
    collect("tag", filter.tags);
    collect("exclude_tag", filter.excludeTags);
    collect("exclude_suite", filter.excludeSuites);
    return filter;
}

/// Reads and bounds an integer query parameter.
int boundedInt(
    const ServerRequest& request, std::string_view name, int fallback, int minimum, int maximum) {
    const int value = request.queryParameterInt(name, fallback);
    return std::clamp(value, minimum, maximum);
}

}  // namespace

RestApiServer::RestApiServer(TestForgeService& service, ServerConfig config)
    : service_(&service), config_(std::move(config)) {
    server_ = std::make_unique<net::HttpServer>(config_);
}

RestApiServer::~RestApiServer() {
    stop();
}

void RestApiServer::registerRoutes() {
    net::HttpServer& server = *server_;
    TestForgeService& service = *service_;

    server.route("GET", "/api/health", [&service](const ServerRequest&) {
        return ServerResponse::json(200, service.healthJson());
    });

    server.route("GET", "/api/tests", [&service](const ServerRequest& request) {
        return ServerResponse::json(200, service.listTestsJson(filterFromQuery(request)));
    });

    server.route("GET", "/api/suites", [&service](const ServerRequest&) {
        return ServerResponse::json(200, service.suitesJson());
    });

    // ---- runs ------------------------------------------------------------

    server.route("POST", "/api/runs", [&service](const ServerRequest& request) {
        if (service.runInProgress()) {
            // 409 rather than queueing: a caller that wanted to wait can retry,
            // and a caller that did not needs to know immediately.
            return ServerResponse::error(409, "a run is already in progress", "RUN_IN_PROGRESS");
        }

        json::Value body = json::Value::object();
        if (!request.body.empty()) {
            const std::optional<json::Value> parsed = request.json();
            if (!parsed.has_value()) {
                return ServerResponse::error(
                    400, "the request body is not valid JSON", "INVALID_JSON");
            }
            body = *parsed;
        }

        RunOptions options;
        options.filter = filterFromBody(body);
        options.label = body.find("label") != nullptr ? json::stringAt(body, "label", "") : "";
        if (const json::Value* workers = body.find("workers");
            workers != nullptr && workers->isNumber()) {
            options.workers = std::clamp(static_cast<int>(workers->asInt()), 1, 128);
        }
        if (const json::Value* failFast = body.find("fail_fast"); failFast != nullptr) {
            options.failFast = failFast->boolOr(false);
        }
        if (const json::Value* retries = body.find("retry_failed");
            retries != nullptr && retries->isNumber()) {
            options.retryFailed = std::clamp(static_cast<int>(retries->asInt()), 0, 5);
        }
        if (const json::Value* shuffle = body.find("shuffle"); shuffle != nullptr) {
            options.selection.shuffle = shuffle->boolOr(false);
        }

        // Synchronous by design. An async job API would need a job store, a
        // polling endpoint and cancellation semantics; for a tool whose runs
        // take seconds, that is complexity without a payoff. Long runs should
        // use the CLI.
        const TestRun run = service.run(options);

        json::Value out = run.toJson();
        // 200 even when tests failed: the API call succeeded, and the verdict
        // is in the body. A 4xx/5xx here would conflate "the request was bad"
        // with "the system under test is broken".
        out.set("ok", true);
        out.set("tests_passed", run.exitCode() == 0);
        return ServerResponse::json(200, out);
    });

    server.route("GET", "/api/runs", [&service](const ServerRequest& request) {
        return ServerResponse::json(200,
                                    service.historyJson(boundedInt(request, "limit", 25, 1, 500)));
    });

    server.route("GET", "/api/runs/:id", [&service](const ServerRequest& request) {
        const json::Value out = service.runJson(request.pathParameter("id"));
        const bool ok = out.find("ok") != nullptr && json::boolAt(out, "ok", false);
        return ServerResponse::json(ok ? 200 : 404, out);
    });

    server.route("GET", "/api/runs/:id/results", [&service](const ServerRequest& request) {
        return ServerResponse::json(200, service.runResultsJson(request.pathParameter("id")));
    });

    // ---- analytics -------------------------------------------------------

    server.route("GET", "/api/stats", [&service](const ServerRequest& request) {
        return ServerResponse::json(200,
                                    service.statsJson(boundedInt(request, "runs", 50, 1, 1000)));
    });

    server.route("GET", "/api/history", [&service](const ServerRequest& request) {
        const std::string test = request.queryParameter("test");
        const int limit = boundedInt(request, "limit", 25, 1, 500);
        if (test.empty()) {
            return ServerResponse::json(200, service.historyJson(limit));
        }
        return ServerResponse::json(200, service.testHistoryJson(test, limit));
    });

    server.route("GET", "/api/flaky", [&service](const ServerRequest& request) {
        return ServerResponse::json(
            200, service.flakyCandidatesJson(boundedInt(request, "limit", 20, 1, 200)));
    });

    // ---- diagnostics -----------------------------------------------------

    server.route("GET", "/api/diagnostics", [&service](const ServerRequest&) {
        return ServerResponse::json(200, service.diagnosticsJson());
    });

    server.route("GET", "/api/diagnostics/gpu", [&service](const ServerRequest&) {
        return ServerResponse::json(200, service.gpuJson());
    });

    // ---- AI --------------------------------------------------------------

    server.route("GET", "/api/ai/status", [&service](const ServerRequest&) {
        json::Value out = json::Value::object();
        out.set("ok", true);
        if (const ai::AiProviderPtr provider = service.aiProvider()) {
            out.set("ai", provider->health());
        } else {
            out.set("ai", json::Value::object().set("enabled", false));
        }
        return ServerResponse::json(200, out);
    });

    server.route("POST", "/api/ai/generate-tests", [&service](const ServerRequest& request) {
        const std::optional<json::Value> body = request.json();
        if (!body.has_value()) {
            return ServerResponse::error(
                400, "the request body must be a JSON object", "INVALID_JSON");
        }
        const std::string requirement =
            body->find("requirement") != nullptr ? json::stringAt(*body, "requirement", "") : "";
        const std::string suite = body->find("suite") != nullptr
                                      ? json::stringAt(*body, "suite", "generated")
                                      : "generated";
        const int maxTests = body->find("max_tests") != nullptr
                                 ? static_cast<int>(json::intAt(*body, "max_tests", 10))
                                 : 10;
        const bool registerSuite =
            body->find("register") != nullptr && json::boolAt(*body, "register", false);

        const json::Value out = service.generateTests(requirement, suite, maxTests, registerSuite);
        const bool ok = out.find("ok") != nullptr && json::boolAt(out, "ok", false);
        return ServerResponse::json(ok ? 200 : 422, out);
    });

    server.route("POST", "/api/ai/analyze-failure", [&service](const ServerRequest& request) {
        const std::optional<json::Value> body = request.json();
        if (!body.has_value()) {
            return ServerResponse::error(
                400, "the request body must be a JSON object", "INVALID_JSON");
        }
        const std::string runId =
            body->find("run_id") != nullptr ? json::stringAt(*body, "run_id", "") : "";
        const std::string test =
            body->find("test") != nullptr ? json::stringAt(*body, "test", "") : "";
        if (runId.empty()) {
            return ServerResponse::error(400, "'run_id' is required", "INVALID_REQUEST");
        }
        const json::Value out = service.analyseFailure(runId, test);
        const bool ok = out.find("ok") != nullptr && json::boolAt(out, "ok", false);
        return ServerResponse::json(ok ? 200 : 404, out);
    });

    // ---- convenience -----------------------------------------------------

    // A bare /api listing, so somebody poking at the port can discover it.
    server.route("GET", "/api", [](const ServerRequest&) {
        json::Value routes = json::Value::array();
        for (const std::string_view route : {"GET /api/health",
                                             "GET /api/tests",
                                             "GET /api/suites",
                                             "POST /api/runs",
                                             "GET /api/runs",
                                             "GET /api/runs/{id}",
                                             "GET /api/runs/{id}/results",
                                             "GET /api/stats",
                                             "GET /api/history",
                                             "GET /api/flaky",
                                             "GET /api/diagnostics",
                                             "GET /api/diagnostics/gpu",
                                             "GET /api/ai/status",
                                             "POST /api/ai/generate-tests",
                                             "POST /api/ai/analyze-failure"}) {
            routes.push(std::string(route));
        }
        json::Value out = json::Value::object();
        out.set("service", "testforge");
        out.set("routes", routes);
        return ServerResponse::json(200, out);
    });
}

bool RestApiServer::start() {
    registerRoutes();

    if (!config_.staticDirectory.empty()) {
        server_->serveStaticFiles(config_.staticDirectory, "/");
    }

    if (!server_->start()) {
        return false;
    }

    logger().info("REST API listening",
                  {{"address", address()}, {"dashboard", config_.staticDirectory}});
    return true;
}

void RestApiServer::stop() {
    if (server_) {
        server_->stop();
    }
}

void RestApiServer::wait() {
    if (server_) {
        server_->wait();
    }
}

bool RestApiServer::waitFor(Milliseconds timeout) {
    return !server_ || server_->waitFor(timeout);
}

int RestApiServer::boundPort() const noexcept {
    return server_ ? server_->boundPort() : 0;
}

bool RestApiServer::running() const noexcept {
    return server_ && server_->running();
}

std::string RestApiServer::address() const {
    return "http://" + config_.host + ":" + std::to_string(boundPort());
}

}  // namespace testforge::api
