/// Tests for URL parsing, the URL policy, and HTTP value types.

#include "testforge/core/Exceptions.hpp"
#include "testforge/net/HttpTypes.hpp"
#include "testforge/net/Url.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace testforge;
using namespace testforge::net;

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------

TEST(Url, ParsesTheCommonShapes) {
    const auto url = parseUrl("http://example.test:8080/path/to/thing?a=1&b=2#frag");
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(url->scheme, "http");
    EXPECT_EQ(url->host, "example.test");
    EXPECT_EQ(url->port, 8080);
    EXPECT_EQ(url->path, "/path/to/thing");
    EXPECT_EQ(url->query, "a=1&b=2");
    EXPECT_EQ(url->fragment, "frag");
    EXPECT_EQ(url->requestTarget(), "/path/to/thing?a=1&b=2");
}

TEST(Url, DefaultPortsBySchemeAndAreOmittedFromTheAuthority) {
    EXPECT_EQ(parseUrl("http://example.test/")->port, 80);
    EXPECT_EQ(parseUrl("https://example.test/")->port, 443);
    EXPECT_EQ(parseUrl("http://example.test/")->authority(), "example.test");
    EXPECT_EQ(parseUrl("http://example.test:8080/")->authority(), "example.test:8080");
}

TEST(Url, MissingPathBecomesRoot) {
    EXPECT_EQ(parseUrl("http://example.test")->path, "/");
    EXPECT_EQ(parseUrl("http://example.test")->requestTarget(), "/");
}

TEST(Url, HostIsLowercased) {
    EXPECT_EQ(parseUrl("HTTP://Example.TEST/Path")->host, "example.test");
    // The path keeps its case: paths are case-sensitive, hosts are not.
    EXPECT_EQ(parseUrl("HTTP://Example.TEST/Path")->path, "/Path");
}

TEST(Url, Ipv6Literals) {
    const auto url = parseUrl("http://[::1]:9000/health");
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(url->host, "::1");
    EXPECT_EQ(url->port, 9000);
}

TEST(Url, CredentialsInTheUrlAreDiscarded) {
    // Credentials in a URL end up in logs; TestForge drops them rather than
    // carrying them around.
    const auto url = parseUrl("http://user:secret@example.test/path");
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(url->host, "example.test");
    EXPECT_EQ(url->toString().find("secret"), std::string::npos);
}

TEST(Url, RejectsMalformed) {
    for (const char* bad : {"",
                            "not-a-url",
                            "://missing-scheme",
                            "http://",
                            "http://host:notaport/",
                            "http://host:0/",
                            "http://host:99999/"}) {
        EXPECT_FALSE(parseUrl(bad).has_value()) << bad;
    }
}

TEST(Url, ToStringRoundTrips) {
    const std::string source = "http://example.test:8080/a/b?x=1";
    EXPECT_EQ(parseUrl(source)->toString(), source);
}

// ---------------------------------------------------------------------------
// Joining
// ---------------------------------------------------------------------------

TEST(UrlJoin, AppendsRelativePaths) {
    EXPECT_EQ(*joinUrl("http://host:8000", "/users"), "http://host:8000/users");
    EXPECT_EQ(*joinUrl("http://host:8000/", "/users"), "http://host:8000/users");
    EXPECT_EQ(*joinUrl("http://host:8000/api", "/users"), "http://host:8000/api/users");
    EXPECT_EQ(*joinUrl("http://host:8000", "users"), "http://host:8000/users");
}

TEST(UrlJoin, CarriesTheQueryString) {
    EXPECT_EQ(*joinUrl("http://host:8000", "/items?limit=5"), "http://host:8000/items?limit=5");
}

TEST(UrlJoin, AnAbsoluteUrlOverridesTheBase) {
    EXPECT_EQ(*joinUrl("http://host:8000", "http://other:9000/x"), "http://other:9000/x");
}

TEST(UrlJoin, RejectsTraversalRatherThanNormalisingIt) {
    // Resolving "../.." silently would turn a typo in a generated spec into a
    // request nobody intended.
    EXPECT_FALSE(joinUrl("http://host:8000/api", "/../admin").has_value());
    EXPECT_FALSE(joinUrl("http://host:8000/api", "..").has_value());
}

TEST(UrlEncoding, RoundTrips) {
    const std::string raw = "a b&c=d/e?f#g+h";
    const std::string encoded = urlEncode(raw);
    EXPECT_EQ(encoded.find(' '), std::string::npos);
    EXPECT_EQ(urlDecode(encoded), raw);
}

TEST(UrlQuery, ParsesPairs) {
    const auto pairs = parseQuery("a=1&b=hello%20world&c&d=");
    ASSERT_EQ(pairs.size(), 4U);
    EXPECT_EQ(pairs[0].first, "a");
    EXPECT_EQ(pairs[1].second, "hello world");
    EXPECT_EQ(pairs[2].second, "");
}

// ---------------------------------------------------------------------------
// URL policy — the SSRF guard
// ---------------------------------------------------------------------------

TEST(UrlPolicy, AllowsOrdinaryHttpByDefault) {
    const UrlPolicy policy;
    EXPECT_EQ(policy.reject("http://127.0.0.1:8000/health"), "");
    EXPECT_EQ(policy.reject("https://example.test/x"), "");
}

TEST(UrlPolicy, RejectsOtherSchemes) {
    const UrlPolicy policy;
    EXPECT_NE(policy.reject("file:///etc/passwd"), "");
    EXPECT_NE(policy.reject("ftp://example.test/x"), "");
    EXPECT_NE(policy.reject("not a url"), "");
}

TEST(UrlPolicy, BlocksCloudMetadataByDefault) {
    // 169.254.169.254 is the metadata endpoint on AWS, GCP and Azure, and is
    // the classic SSRF target.
    const UrlPolicy policy;
    const std::string rejection = policy.reject("http://169.254.169.254/latest/meta-data/");
    EXPECT_NE(rejection, "");
    EXPECT_NE(rejection.find("link-local"), std::string::npos);
}

TEST(UrlPolicy, LoopbackIsAllowedUnlessAskedOtherwise) {
    UrlPolicy policy;
    EXPECT_EQ(policy.reject("http://localhost:8000/"), "");

    policy.blockLoopback = true;
    EXPECT_NE(policy.reject("http://localhost:8000/"), "");
    EXPECT_NE(policy.reject("http://127.0.0.1:8000/"), "");
}

TEST(UrlPolicy, PrivateRangesAreOptional) {
    UrlPolicy policy;
    EXPECT_EQ(policy.reject("http://10.0.0.5/"), "");

    policy.blockPrivateNetworks = true;
    EXPECT_NE(policy.reject("http://10.0.0.5/"), "");
    EXPECT_NE(policy.reject("http://192.168.1.1/"), "");
    EXPECT_NE(policy.reject("http://172.16.0.1/"), "");
    // 172.32 is outside the RFC 1918 block.
    EXPECT_EQ(policy.reject("http://172.32.0.1/"), "");
}

TEST(UrlPolicy, AllowListPinsToOneHost) {
    const UrlPolicy policy = UrlPolicy::restrictedTo("http://127.0.0.1:8000");
    EXPECT_EQ(policy.reject("http://127.0.0.1:8000/anything"), "");
    // A different port on the same host is fine; a different host is not.
    EXPECT_EQ(policy.reject("http://127.0.0.1:9999/x"), "");
    EXPECT_NE(policy.reject("http://evil.test/x"), "");
}

// ---------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------

TEST(HttpHeaders, LookupIsCaseInsensitive) {
    HttpHeaders headers;
    headers.set("Content-Type", "application/json");
    EXPECT_EQ(headers.getOr("content-type", ""), "application/json");
    EXPECT_EQ(headers.getOr("CONTENT-TYPE", ""), "application/json");
    EXPECT_TRUE(headers.contains("Content-Type"));
    EXPECT_FALSE(headers.contains("Accept"));
}

TEST(HttpHeaders, SetReplacesAndAddAccumulates) {
    HttpHeaders headers;
    headers.add("Set-Cookie", "a=1");
    headers.add("Set-Cookie", "b=2");
    EXPECT_EQ(headers.getAll("set-cookie").size(), 2U);

    headers.set("Set-Cookie", "c=3");
    EXPECT_EQ(headers.getAll("set-cookie").size(), 1U);
}

TEST(HttpHeaders, RedactedJsonHidesCredentials) {
    HttpHeaders headers;
    headers.set("Authorization",
                // NOT-A-SECRET: fixture string, never a live credential
                "Bearer sk-abcdef1234567890");
    headers.set("Content-Type", "application/json");

    const json::Value document = headers.toRedactedJson();
    EXPECT_EQ(document.at("Authorization").asString(), "***REDACTED***");
    EXPECT_EQ(document.at("Content-Type").asString(), "application/json");
}

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------

TEST(HttpResponse, StatusClassification) {
    HttpResponse response;
    response.statusCode = 204;
    EXPECT_TRUE(response.ok());
    response.statusCode = 404;
    EXPECT_TRUE(response.isClientError());
    EXPECT_FALSE(response.ok());
    response.statusCode = 503;
    EXPECT_TRUE(response.isServerError());
}

TEST(HttpResponse, JsonParsingIsOptional) {
    HttpResponse response;
    response.body = R"({"a":1})";
    ASSERT_TRUE(response.json().has_value());
    EXPECT_EQ(response.json()->at("a").asInt(), 1);

    response.body = "not json";
    EXPECT_FALSE(response.json().has_value());
    EXPECT_THROW((void)response.jsonOrThrow(), AssertionFailure);

    response.body.clear();
    EXPECT_TRUE(response.jsonOrThrow().isNull());
}

TEST(HttpResponse, ContentTypeDropsParameters) {
    HttpResponse response;
    response.headers.set("Content-Type", "application/json; charset=utf-8");
    EXPECT_EQ(response.contentType(), "application/json");
}

TEST(HttpResponse, MetadataCarriesWhatTheClassifierNeeds) {
    HttpResponse response;
    response.requestMethod = "POST";
    response.requestUrl = "http://host/users";
    response.statusCode = 500;
    response.elapsed = Milliseconds{123};
    response.body = R"({"detail":"boom"})";

    const json::Value metadata = response.toMetadataJson();
    // http_status is the field FailureClassifier::refine reads.
    EXPECT_EQ(metadata.at("http_status").asInt(), 500);
    EXPECT_EQ(metadata.at("response_time_ms").asInt(), 123);
    EXPECT_NE(metadata.at("response_excerpt").asString().find("boom"), std::string::npos);
}

TEST(HttpResponse, MetadataExcerptIsRedactedAndBounded) {
    HttpResponse response;
    response.body =
        // NOT-A-SECRET: fixture string, never a live credential
        R"({"token":"sk-abcdef1234567890","padding":")" + std::string(9000, 'x') + R"("})";
    const json::Value metadata = response.toMetadataJson();
    const std::string excerpt = metadata.at("response_excerpt").asString();

    EXPECT_LE(excerpt.size(), 2100U);
    EXPECT_EQ(excerpt.find("abcdef1234567890"), std::string::npos);
}

TEST(HttpResponse, DescribeTargetIsUsableInAMessage) {
    HttpResponse response;
    response.requestMethod = "GET";
    response.requestUrl = "http://host/health";
    EXPECT_EQ(response.describeTarget(), "GET http://host/health");
}

TEST(HttpRequest, JsonPostSetsTheContentType) {
    json::Value body = json::Value::object();
    body.set("a", 1);
    const HttpRequest request = HttpRequest::jsonPost("http://host/x", body);
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.headers.getOr("Content-Type", ""), "application/json");
    EXPECT_EQ(request.body, R"({"a":1})");
}

TEST(ReasonPhrase, KnownAndUnknownCodes) {
    EXPECT_EQ(reasonPhrase(200), "OK");
    EXPECT_EQ(reasonPhrase(404), "Not Found");
    EXPECT_EQ(reasonPhrase(503), "Service Unavailable");
    EXPECT_EQ(reasonPhrase(499), "Client Error");
    EXPECT_EQ(reasonPhrase(599), "Server Error");
}
