/// Round-trip tests for the HTTP stack.
///
/// The client and the server are both TestForge's own code, so pointing one at
/// the other exercises request parsing, response framing, routing, chunked
/// decoding, timeouts and cancellation without needing an external service.
/// The server binds to port 0, so these tests cannot collide with anything
/// already running.

#include "testforge/core/CancellationToken.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/net/HttpClient.hpp"
#include "testforge/net/HttpServer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace testforge;
using namespace testforge::net;

namespace {

class HttpFixture : public ::testing::Test {
 protected:
    void SetUp() override {
        ServerConfig config;
        config.host = "127.0.0.1";
        config.port = 0;  // any free port
        config.workers = 2;
        config.staticDirectory.clear();

        server = std::make_unique<HttpServer>(config);
        registerRoutes();
        ASSERT_TRUE(server->start()) << "could not bind a loopback port";
        base = "http://127.0.0.1:" + std::to_string(server->boundPort());

        HttpClientOptions clientOptions;
        clientOptions.requestTimeout = Milliseconds{5000};
        clientOptions.connectTimeout = Milliseconds{2000};
        client = std::make_unique<HttpClient>(clientOptions);
    }

    void TearDown() override {
        if (server) {
            server->stop();
        }
    }

    void registerRoutes() {
        server->route("GET", "/health", [](const ServerRequest&) {
            json::Value body = json::Value::object();
            body.set("status", "ok");
            return ServerResponse::json(200, body);
        });

        server->route("GET", "/echo", [](const ServerRequest& request) {
            json::Value body = json::Value::object();
            body.set("method", request.method);
            body.set("path", request.path);
            body.set("query", request.query);
            body.set("limit", request.queryParameterInt("limit", -1));
            body.set("agent", request.headers.getOr("User-Agent", ""));
            return ServerResponse::json(200, body);
        });

        server->route("POST", "/echo-body", [](const ServerRequest& request) {
            json::Value body = json::Value::object();
            body.set("received", request.body);
            body.set("bytes", static_cast<std::int64_t>(request.body.size()));
            const auto parsed = request.json();
            body.set("was_json", parsed.has_value());
            return ServerResponse::json(200, body);
        });

        server->route("GET", "/items/:id", [](const ServerRequest& request) {
            json::Value body = json::Value::object();
            body.set("id", request.pathParameter("id"));
            return ServerResponse::json(200, body);
        });

        server->route("GET", "/status/:code", [](const ServerRequest& request) {
            std::int64_t code = 200;
            strings::parseInt(request.pathParameter("code"), code);
            return ServerResponse::json(static_cast<int>(code), json::Value::object());
        });

        server->route(
            "DELETE", "/thing", [](const ServerRequest&) { return ServerResponse::noContent(); });

        server->route("GET", "/slow", [](const ServerRequest& request) {
            const int delay = request.queryParameterInt("ms", 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            return ServerResponse::text(200, "done");
        });

        server->route("GET", "/large", [](const ServerRequest& request) {
            const int size = request.queryParameterInt("bytes", 100000);
            return ServerResponse::text(200, std::string(static_cast<std::size_t>(size), 'x'));
        });

        server->route("GET", "/throws", [](const ServerRequest&) -> ServerResponse {
            throw std::runtime_error("a handler blew up");
        });
    }

    std::unique_ptr<HttpServer> server;
    std::unique_ptr<HttpClient> client;
    std::string base;
};

}  // namespace

// ---------------------------------------------------------------------------
// Basics
// ---------------------------------------------------------------------------

TEST_F(HttpFixture, GetReturnsJson) {
    const HttpResponse response = client->get(base + "/health");

    ASSERT_FALSE(response.transportError) << response.errorMessage;
    EXPECT_EQ(response.statusCode, 200);
    EXPECT_TRUE(response.ok());
    EXPECT_EQ(response.contentType(), "application/json");

    const json::Value body = response.jsonOrThrow();
    EXPECT_EQ(body.at("status").asString(), "ok");
    EXPECT_GE(response.elapsed.count(), 0);
}

TEST_F(HttpFixture, QueryStringAndHeadersArrive) {
    const HttpResponse response = client->get(base + "/echo?limit=5&other=x");
    ASSERT_FALSE(response.transportError) << response.errorMessage;

    const json::Value body = response.jsonOrThrow();
    EXPECT_EQ(body.at("path").asString(), "/echo");
    EXPECT_EQ(body.at("limit").asInt(), 5);
    EXPECT_NE(body.at("agent").asString().find("TestForge"), std::string::npos);
}

TEST_F(HttpFixture, PathParametersAreCaptured) {
    const HttpResponse response = client->get(base + "/items/42");
    ASSERT_FALSE(response.transportError) << response.errorMessage;
    EXPECT_EQ(response.jsonOrThrow().at("id").asString(), "42");
}

TEST_F(HttpFixture, PostCarriesItsBody) {
    json::Value payload = json::Value::object();
    payload.set("name", "value");

    const HttpResponse response = client->postJson(base + "/echo-body", payload);
    ASSERT_FALSE(response.transportError) << response.errorMessage;

    const json::Value body = response.jsonOrThrow();
    EXPECT_TRUE(body.at("was_json").asBool());
    EXPECT_EQ(body.at("received").asString(), R"({"name":"value"})");
}

TEST_F(HttpFixture, EveryVerbIsRouted) {
    EXPECT_EQ(client->del(base + "/thing").statusCode, 204);
    // A 204 must carry no body.
    EXPECT_TRUE(client->del(base + "/thing").body.empty());
}

TEST_F(HttpFixture, StatusCodesPassThrough) {
    for (const int code : {200, 201, 400, 404, 422, 500, 503}) {
        const HttpResponse response = client->get(base + "/status/" + std::to_string(code));
        ASSERT_FALSE(response.transportError) << response.errorMessage;
        EXPECT_EQ(response.statusCode, code);
    }
}

TEST_F(HttpFixture, UnknownRouteIs404AndWrongVerbIs405) {
    const HttpResponse missing = client->get(base + "/no-such-route");
    EXPECT_EQ(missing.statusCode, 404);

    // /health exists but only for GET, so this is 405 rather than 404 — the
    // distinction tells the caller they got the path right.
    const HttpResponse wrongVerb = client->post(base + "/health", "{}");
    EXPECT_EQ(wrongVerb.statusCode, 405);
}

TEST_F(HttpFixture, AThrowingHandlerBecomesA500NotACrash) {
    const HttpResponse response = client->get(base + "/throws");
    ASSERT_FALSE(response.transportError) << response.errorMessage;
    EXPECT_EQ(response.statusCode, 500);

    // The server must still be serving afterwards.
    EXPECT_EQ(client->get(base + "/health").statusCode, 200);
}

TEST_F(HttpFixture, LargeResponsesAreReadCompletely) {
    const HttpResponse response = client->get(base + "/large?bytes=250000");
    ASSERT_FALSE(response.transportError) << response.errorMessage;
    EXPECT_EQ(response.body.size(), 250000U);
}

TEST_F(HttpFixture, ResponseSizeIsCapped) {
    HttpClientOptions options;
    options.maxResponseBytes = 1024;
    HttpClient limited(options);

    const HttpResponse response = limited.get(base + "/large?bytes=100000");
    ASSERT_FALSE(response.transportError) << response.errorMessage;
    // A misbehaving endpoint must not be able to exhaust memory.
    EXPECT_LE(response.body.size(), 1024U);
    EXPECT_TRUE(response.headers.contains("X-TestForge-Truncated"));
}

// ---------------------------------------------------------------------------
// Failure modes
// ---------------------------------------------------------------------------

TEST_F(HttpFixture, ConnectionRefusedIsATransportErrorNotAnException) {
    // Port 9 (discard) is reserved and nothing listens on it.
    HttpClientOptions options;
    options.connectTimeout = Milliseconds{1500};
    HttpClient other(options);

    const HttpResponse response = other.get("http://127.0.0.1:9/nothing");
    EXPECT_TRUE(response.transportError);
    EXPECT_EQ(response.statusCode, 0);
    EXPECT_FALSE(response.errorMessage.empty());
}

TEST_F(HttpFixture, RequestTimeoutIsEnforced) {
    HttpClientOptions options;
    options.requestTimeout = Milliseconds{200};
    HttpClient impatient(options);

    const Stopwatch watch;
    const HttpResponse response = impatient.get(base + "/slow?ms=3000");
    const std::int64_t elapsed = watch.elapsed().count();

    EXPECT_TRUE(response.transportError);
    EXPECT_LT(elapsed, 2500) << "the client waited past its own deadline";
}

TEST_F(HttpFixture, CancellationAbortsAnInFlightRequest) {
    // This is what lets a timed-out test stop instead of blocking its worker
    // on a socket.
    CancellationSource source;
    const CancellationToken token = source.token();

    std::thread canceller([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        source.cancel("test deadline");
    });

    HttpRequest request = HttpRequest::get(base + "/slow?ms=5000");
    const Stopwatch watch;
    const HttpResponse response = client->send(request, token);
    const std::int64_t elapsed = watch.elapsed().count();
    canceller.join();

    EXPECT_TRUE(response.transportError);
    EXPECT_LT(elapsed, 3000);
    EXPECT_NE(response.errorMessage.find("abort"), std::string::npos);
}

TEST_F(HttpFixture, HttpsIsRefusedWithAClearExplanation) {
    const HttpResponse response = client->get("https://example.test/");
    EXPECT_TRUE(response.transportError);
    // A precise message beats a confusing handshake failure.
    EXPECT_NE(response.errorMessage.find("HTTPS is not supported"), std::string::npos);
}

TEST_F(HttpFixture, UrlPolicyBlocksBeforeASocketIsOpened) {
    HttpClientOptions options;
    options.policy = UrlPolicy::restrictedTo(base);
    HttpClient restricted(options);

    EXPECT_FALSE(restricted.get(base + "/health").transportError);

    const HttpResponse blocked = restricted.get("http://169.254.169.254/latest/meta-data/");
    EXPECT_TRUE(blocked.transportError);
    EXPECT_NE(blocked.errorMessage.find("policy"), std::string::npos);
}

TEST_F(HttpFixture, InvalidUrlsAreRejected) {
    const HttpResponse response = client->get("not-a-url");
    EXPECT_TRUE(response.transportError);
    EXPECT_EQ(response.statusCode, 0);
}

// ---------------------------------------------------------------------------
// Server behaviour
// ---------------------------------------------------------------------------

TEST_F(HttpFixture, HandlesConcurrentClients) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 10;
    std::atomic<int> succeeded{0};

    std::vector<std::thread> callers;
    for (int t = 0; t < kThreads; ++t) {
        callers.emplace_back([this, &succeeded] {
            HttpClient localClient;
            for (int i = 0; i < kPerThread; ++i) {
                if (localClient.get(base + "/health").statusCode == 200) {
                    succeeded.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& caller : callers) {
        caller.join();
    }

    EXPECT_EQ(succeeded.load(), kThreads * kPerThread);
    EXPECT_GE(server->requestsHandled(), static_cast<std::uint64_t>(kThreads * kPerThread));
}

TEST_F(HttpFixture, OversizedRequestsAreRejected) {
    ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.workers = 1;
    config.maxRequestBytes = 1024;
    config.staticDirectory.clear();

    HttpServer small(config);
    small.route("POST", "/x", [](const ServerRequest&) { return ServerResponse::text(200, "ok"); });
    ASSERT_TRUE(small.start());

    const std::string address = "http://127.0.0.1:" + std::to_string(small.boundPort());
    const HttpResponse response = client->post(address + "/x", std::string(50000, 'x'));
    EXPECT_EQ(response.statusCode, 413);

    small.stop();
}

TEST_F(HttpFixture, RoutesCanBeReplaced) {
    server->route("GET", "/health", [](const ServerRequest&) {
        return ServerResponse::text(200, "replaced");
    });
    EXPECT_EQ(client->get(base + "/health").body, "replaced");
}

TEST_F(HttpFixture, PortZeroBindsSomethingReal) {
    EXPECT_GT(server->boundPort(), 0);
    EXPECT_TRUE(server->running());
}

TEST_F(HttpFixture, StopIsIdempotent) {
    server->stop();
    EXPECT_FALSE(server->running());
    EXPECT_NO_THROW(server->stop());
}

// ---------------------------------------------------------------------------
// Credentials across redirects
// ---------------------------------------------------------------------------
//
// A redirect is followed by copying the original request, headers and all. If
// the target is a different origin, that copy would hand its operator the
// Authorization or Cookie the caller attached for the *first* host. curl gates
// exactly this behind --location-trusted; TestForge refuses it outright.
//
// Two servers on two loopback ports give two genuinely different origins: the
// port is part of the origin, so 127.0.0.1:A and 127.0.0.1:B are no more the
// same origin than two unrelated hosts would be.

namespace {

class RedirectFixture : public ::testing::Test {
 protected:
    void SetUp() override {
        alphaServer = create();
        bravoServer = create();

        // Routes are registered before either server starts: the route table
        // is read by the acceptor's workers without a lock, so adding to it
        // afterwards is a data race (and produced intermittent 404s).
        //
        // The cross-origin targets are therefore read from a member at request
        // time rather than baked in at registration time, because neither
        // server has a port yet.
        const auto landing = [](const ServerRequest& request) {
            json::Value body = json::Value::object();
            body.set("authorization", request.headers.getOr("Authorization", ""));
            body.set("cookie", request.headers.getOr("Cookie", ""));
            body.set("custom", request.headers.getOr("X-Trace-Id", ""));
            return ServerResponse::json(200, body);
        };
        alphaServer->route("GET", "/landing", landing);
        bravoServer->route("GET", "/landing", landing);

        // `fixed` is a path on the same server; `origin` names a member whose
        // value is filled in once that server has bound a port.
        const auto redirectToPath = [](std::string path) {
            return [path](const ServerRequest&) {
                ServerResponse response;
                response.status = 302;
                response.headers.set("Location", path);
                return response;
            };
        };
        const auto redirectToOther = [](const std::string* origin, std::string path) {
            return [origin, path](const ServerRequest&) {
                ServerResponse response;
                response.status = 302;
                response.headers.set("Location", *origin + path);
                return response;
            };
        };

        alphaServer->route("GET", "/same-origin", redirectToPath("/landing"));
        alphaServer->route("GET", "/cross-origin", redirectToOther(&bravo, "/landing"));
        alphaServer->route("GET", "/chain", redirectToPath("/chain-hop"));
        alphaServer->route("GET", "/chain-hop", redirectToOther(&bravo, "/landing"));
        alphaServer->route("GET", "/out-and-back", redirectToOther(&bravo, "/back-to-alpha"));
        bravoServer->route("GET", "/back-to-alpha", redirectToOther(&alpha, "/landing"));

        alpha = startAndDescribe(*alphaServer);
        bravo = startAndDescribe(*bravoServer);
        ASSERT_FALSE(alpha.empty());
        ASSERT_FALSE(bravo.empty());

        HttpClientOptions clientOptions;
        clientOptions.requestTimeout = Milliseconds{5000};
        clientOptions.connectTimeout = Milliseconds{2000};
        clientOptions.followRedirects = true;
        client = std::make_unique<HttpClient>(clientOptions);
    }

    void TearDown() override {
        if (alphaServer) {
            alphaServer->stop();
        }
        if (bravoServer) {
            bravoServer->stop();
        }
    }

    static std::unique_ptr<HttpServer> create() {
        ServerConfig config;
        config.host = "127.0.0.1";
        config.port = 0;
        config.workers = 2;
        config.staticDirectory.clear();
        return std::make_unique<HttpServer>(config);
    }

    static std::string startAndDescribe(HttpServer& server) {
        if (!server.start()) {
            return {};
        }
        return "http://127.0.0.1:" + std::to_string(server.boundPort());
    }

    /// A request carrying both credential kinds plus one ordinary header.
    [[nodiscard]] HttpResponse fetch(const std::string& path) const {
        HttpRequest request;
        request.method = "GET";
        request.url = alpha + path;
        request.headers.set("Authorization", "Bearer secret-token-value");
        request.headers.set("Cookie", "session=secret-session-value");
        request.headers.set("X-Trace-Id", "trace-42");
        return client->send(request, CancellationToken{});
    }

    std::unique_ptr<HttpServer> alphaServer;
    std::unique_ptr<HttpServer> bravoServer;
    std::string alpha;
    std::string bravo;
    std::unique_ptr<HttpClient> client;
};

}  // namespace

TEST_F(RedirectFixture, SameOriginRedirectKeepsCredentials) {
    const HttpResponse response = fetch("/same-origin");
    ASSERT_EQ(response.statusCode, 200) << response.errorMessage;

    const json::Value body = json::parse(response.body);
    EXPECT_EQ(json::stringAt(body, "authorization", ""), "Bearer secret-token-value");
    EXPECT_EQ(json::stringAt(body, "cookie", ""), "session=secret-session-value");
}

TEST_F(RedirectFixture, CrossOriginRedirectDropsAuthorization) {
    const HttpResponse response = fetch("/cross-origin");
    ASSERT_EQ(response.statusCode, 200) << response.errorMessage;

    const json::Value body = json::parse(response.body);
    EXPECT_EQ(json::stringAt(body, "authorization", ""), "")
        << "Authorization was forwarded to a different origin";
}

TEST_F(RedirectFixture, CrossOriginRedirectDropsCookie) {
    const HttpResponse response = fetch("/cross-origin");
    ASSERT_EQ(response.statusCode, 200) << response.errorMessage;

    const json::Value body = json::parse(response.body);
    EXPECT_EQ(json::stringAt(body, "cookie", ""), "")
        << "Cookie was forwarded to a different origin";
}

TEST_F(RedirectFixture, CrossOriginRedirectKeepsOrdinaryHeaders) {
    // The rule is about credentials, not about headers in general. Stripping
    // everything would quietly break tracing and content negotiation.
    const HttpResponse response = fetch("/cross-origin");
    ASSERT_EQ(response.statusCode, 200) << response.errorMessage;

    const json::Value body = json::parse(response.body);
    EXPECT_EQ(json::stringAt(body, "custom", ""), "trace-42");
}

TEST_F(RedirectFixture, CredentialsSurviveASameOriginChainAndStopAtTheBoundary) {
    // alpha/chain -> alpha/chain-hop -> bravo/landing.
    // The first hop is same origin, so the check must not fire early; the
    // second leaves the origin, so the credentials must not arrive.
    const HttpResponse response = fetch("/chain");
    ASSERT_EQ(response.statusCode, 200) << response.errorMessage;

    const json::Value body = json::parse(response.body);
    EXPECT_EQ(json::stringAt(body, "authorization", ""), "");
    EXPECT_EQ(json::stringAt(body, "cookie", ""), "");
    EXPECT_EQ(json::stringAt(body, "custom", ""), "trace-42");
}

TEST_F(RedirectFixture, CredentialsAreNotRestoredWhenAChainReturnsToTheOrigin) {
    // alpha/out-and-back -> bravo/back-to-alpha -> alpha/landing.
    // Once dropped, a credential is gone: it must not reappear just because a
    // later hop happens to land back on the original host. Anything else would
    // let a hostile intermediary bounce a request home and read the header.
    const HttpResponse response = fetch("/out-and-back");
    ASSERT_EQ(response.statusCode, 200) << response.errorMessage;

    const json::Value body = json::parse(response.body);
    EXPECT_EQ(json::stringAt(body, "authorization", ""), "")
        << "a credential came back from the dead after leaving its origin";
    EXPECT_EQ(json::stringAt(body, "cookie", ""), "");
}

TEST_F(RedirectFixture, ARequestWithoutCredentialsIsUnaffected) {
    HttpRequest request;
    request.method = "GET";
    request.url = alpha + "/cross-origin";
    request.headers.set("X-Trace-Id", "trace-42");

    const HttpResponse response = client->send(request, CancellationToken{});
    ASSERT_EQ(response.statusCode, 200) << response.errorMessage;

    const json::Value body = json::parse(response.body);
    EXPECT_EQ(json::stringAt(body, "custom", ""), "trace-42");
}

// ---------------------------------------------------------------------------
// Bounding what the server will accept
// ---------------------------------------------------------------------------

TEST(HttpServerLimits, ConnectionsBeyondTheCapAreRefusedRatherThanQueued) {
    // The worker pool's queue has no depth limit, so every accepted-but-
    // unhandled connection is a descriptor the process holds for up to the
    // socket read timeout. Without a cap a client can open sockets faster than
    // the workers drain them and exhaust the descriptor table. A 503 is a
    // bounded, visible failure instead.
    ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.workers = 2;
    config.staticDirectory.clear();
    config.maxPendingConnections = 1;

    HttpServer server(config);
    std::atomic<bool> holding{false};
    server.route("GET", "/hold", [&holding](const ServerRequest&) {
        holding.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return ServerResponse::text(200, "done");
    });
    server.route(
        "GET", "/health", [](const ServerRequest&) { return ServerResponse::text(200, "ok"); });
    ASSERT_TRUE(server.start());
    const std::string base = "http://127.0.0.1:" + std::to_string(server.boundPort());

    HttpClientOptions clientOptions;
    clientOptions.requestTimeout = Milliseconds{5000};
    clientOptions.connectTimeout = Milliseconds{2000};
    HttpClient client(clientOptions);

    // Occupy the single permitted slot.
    std::thread occupier([&client, &base] { (void)client.get(base + "/hold"); });
    for (int i = 0; i < 200 && !holding.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(holding.load(std::memory_order_acquire)) << "the holding request never started";

    const HttpResponse refused = client.get(base + "/health");
    occupier.join();

    EXPECT_EQ(refused.statusCode, 503) << refused.errorMessage;
    EXPECT_NE(refused.body.find("SERVER_BUSY"), std::string::npos);
    EXPECT_GE(server.connectionsRejected(), 1U);

    server.stop();
}

TEST(HttpServerLimits, TheCapIsNotAppliedToTrafficWithinIt) {
    // Guard against the cap firing on ordinary sequential traffic: each
    // request must release its slot when it finishes.
    //
    // The cap is comfortably above anything this test generates. A cap of 1
    // would fail here for a legitimate reason: a slot is released when the
    // handler returns, which can be after the client already has its response
    // and has opened the next connection. The property worth pinning is that
    // traffic well inside the cap is never refused.
    ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.workers = 2;
    config.staticDirectory.clear();
    config.maxPendingConnections = 32;

    HttpServer server(config);
    server.route(
        "GET", "/health", [](const ServerRequest&) { return ServerResponse::text(200, "ok"); });
    ASSERT_TRUE(server.start());
    const std::string base = "http://127.0.0.1:" + std::to_string(server.boundPort());

    HttpClient client;
    for (int i = 0; i < 20; ++i) {
        const HttpResponse response = client.get(base + "/health");
        ASSERT_EQ(response.statusCode, 200) << "request " << i << ": " << response.errorMessage;
    }
    EXPECT_EQ(server.connectionsRejected(), 0U);

    server.stop();
}

TEST_F(HttpFixture, ChunkedRequestBodiesAreRejectedWithAClearError) {
    // Only Content-Length is honoured when reading a body, so a chunked
    // request used to reach the handler with an empty body and no sign that
    // anything had gone wrong. Saying so is much better than guessing.
    HttpRequest request;
    request.method = "POST";
    request.url = base + "/echo-body";
    request.body = R"({"value":1})";
    request.headers.set("Transfer-Encoding", "chunked");

    const HttpResponse response = client->send(request, CancellationToken{});

    EXPECT_EQ(response.statusCode, 400) << response.errorMessage;
    EXPECT_NE(response.body.find("UNSUPPORTED_TRANSFER_ENCODING"), std::string::npos);
    EXPECT_NE(response.body.find("Content-Length"), std::string::npos)
        << "the error should say what to do instead";
}

TEST_F(HttpFixture, AnIdentityTransferEncodingIsStillAccepted) {
    // "identity" means no transformation at all, so it is not a body the
    // server is unable to read.
    HttpRequest request;
    request.method = "POST";
    request.url = base + "/echo-body";
    request.body = R"({"value":1})";
    request.headers.set("Transfer-Encoding", "identity");

    const HttpResponse response = client->send(request, CancellationToken{});
    EXPECT_EQ(response.statusCode, 200) << response.errorMessage;
}

TEST_F(HttpFixture, AnOrdinaryRequestWithNoTransferEncodingIsUnaffected) {
    const HttpResponse response = client->postJson(base + "/echo-body", [] {
        json::Value body = json::Value::object();
        body.set("value", 1);
        return body;
    }());
    EXPECT_EQ(response.statusCode, 200) << response.errorMessage;
}
