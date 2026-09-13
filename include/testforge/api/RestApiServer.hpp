#pragma once

#include "testforge/api/TestForgeService.hpp"
#include "testforge/core/Clock.hpp"
#include "testforge/net/HttpServer.hpp"

#include <memory>
#include <string>

namespace testforge::api {

/// HTTP front end for TestForgeService.
///
/// Endpoints:
///
///   GET  /api/health              service and component status
///   GET  /api/tests               the catalogue, with ?suite= &tag= &test=
///   GET  /api/suites              suites and tags with counts
///   POST /api/runs                start a run; body selects the tests
///   GET  /api/runs                recent runs
///   GET  /api/runs/{id}           one run with its results
///   GET  /api/runs/{id}/results   just the results
///   GET  /api/stats               aggregate analytics
///   GET  /api/history?test=       per-test history
///   GET  /api/flaky               flaky candidates (heuristic)
///   GET  /api/diagnostics         full system diagnostics
///   GET  /api/diagnostics/gpu     GPU only
///   POST /api/ai/generate-tests   requirement -> validated specification
///   POST /api/ai/analyze-failure  failure -> advisory analysis
///   GET  /api/ai/status           AI provider health
///
/// Anything not matching a route is served from the dashboard directory.
///
/// Security posture: binds to loopback by default, has no authentication, and
/// is meant for a developer machine or a CI container. `POST /runs` executes
/// registered tests, so exposing this port to an untrusted network would let
/// anyone who can reach it run them. docs/security.md spells this out.
class RestApiServer {
 public:
    RestApiServer(TestForgeService& service, ServerConfig config);

    ~RestApiServer();

    RestApiServer(const RestApiServer&) = delete;
    RestApiServer& operator=(const RestApiServer&) = delete;
    RestApiServer(RestApiServer&&) = delete;
    RestApiServer& operator=(RestApiServer&&) = delete;

    /// Registers routes, serves the dashboard, binds the socket.
    bool start();

    void stop();

    void wait();

    /// Waits up to \p timeout for the server to stop. True if it has.
    [[nodiscard]] bool waitFor(Milliseconds timeout);

    [[nodiscard]] int boundPort() const noexcept;

    [[nodiscard]] bool running() const noexcept;

    /// Base URL a browser should open.
    [[nodiscard]] std::string address() const;

 private:
    void registerRoutes();

    TestForgeService* service_;
    ServerConfig config_;
    std::unique_ptr<net::HttpServer> server_;
};

}  // namespace testforge::api
