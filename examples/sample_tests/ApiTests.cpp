/// API suite — exercises the sample FastAPI service in sample-service/.
///
/// Every test here skips (rather than fails) when the service is not running,
/// because "the developer has not started the service" is not a defect in the
/// service. Start it with:
///
///   uvicorn app.main:app --port 8000 --app-dir sample-service
///
/// Coverage is deliberately mixed: happy paths, negative cases, boundary
/// values, malformed input and error responses.

#include "testforge/core/Ids.hpp"
#include "testforge/core/Json.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/testing/ApiTestCase.hpp"
#include "testforge/testing/ShortAssertions.hpp"
#include "testforge/testing/TestRegistry.hpp"

#include <string>
#include <vector>

using namespace testforge;

namespace {

/// Every API test starts the same way: build a client, and bow out cleanly if
/// there is nothing listening.
ApiClient connect(TestContext& ctx) {
    ApiClient api(ctx);
    api.skipUnlessReachable("/health");
    return api;
}

}  // namespace

// ---------------------------------------------------------------------------
// Health and readiness
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(api,
                    health_check,
                    TestOptions{}
                        .describe("GET /health returns 200 with a healthy status.")
                        .withTags({"api", "smoke"})
                        .withTimeout(10000)) {
    ApiClient api = connect(ctx);

    const net::HttpResponse response = api.get("/health");
    ASSERT_STATUS_CODE(response, 200);
    ASSERT_RESPONSE_TIME(response, api.slaMs());

    const json::Value body = response.jsonOrThrow();
    ASSERT_JSON_EQ(body, "status", "ok");
    ASSERT_JSON_FIELD(body, "service");
}

TESTFORGE_TEST_OPTS(
    api,
    health_reports_content_type,
    TestOptions{}.describe("The health endpoint declares JSON.").withTags({"api"})) {
    ApiClient api = connect(ctx);
    const net::HttpResponse response = api.get("/health");
    ASSERT_STATUS_CODE(response, 200);
    ASSERT_EQ(response.contentType(), std::string("application/json"));
}

// ---------------------------------------------------------------------------
// Reading users
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(api,
                    list_users,
                    TestOptions{}
                        .describe("GET /users returns a paginated collection.")
                        .withTags({"api", "regression"})) {
    ApiClient api = connect(ctx);

    const net::HttpResponse response = api.get("/users");
    ASSERT_STATUS_CODE(response, 200);

    const json::Value body = response.jsonOrThrow();
    ASSERT_JSON_FIELD(body, "items");
    ASSERT_JSON_FIELD(body, "total");
    ASSERT_TRUE(body.at("items").isArray());
    ctx.addMetadata("user_count", body.at("total").intOr(0));
}

TESTFORGE_TEST_OPTS(
    api,
    get_user_by_id,
    TestOptions{}.describe("A seeded user can be fetched by id.").withTags({"api", "regression"})) {
    ApiClient api = connect(ctx);

    const net::HttpResponse response = api.get("/users/1");
    ASSERT_STATUS_CODE(response, 200);

    const json::Value body = response.jsonOrThrow();
    ASSERT_JSON_EQ(body, "id", 1);
    ASSERT_JSON_FIELD(body, "username");
    ASSERT_JSON_FIELD(body, "email");
    // The API must never return a password hash, whatever else it returns.
    ASSERT_FALSE(body.contains("password"));
    ASSERT_FALSE(body.contains("password_hash"));
}

TESTFORGE_TEST_OPTS(api,
                    unknown_user_returns_404,
                    TestOptions{}
                        .describe("A missing id produces 404, not 200 with an empty body.")
                        .withTags({"api", "negative", "regression"})) {
    ApiClient api = connect(ctx);

    const net::HttpResponse response = api.get("/users/999999");
    ASSERT_STATUS_CODE(response, 404);

    const json::Value body = response.jsonOrThrow();
    ASSERT_JSON_FIELD(body, "detail");
}

TESTFORGE_TEST_OPTS(api,
                    non_numeric_id_is_rejected,
                    TestOptions{}
                        .describe("A path parameter of the wrong type produces 422.")
                        .withTags({"api", "negative", "boundary"})) {
    ApiClient api = connect(ctx);

    const net::HttpResponse response = api.get("/users/not-a-number");
    // FastAPI answers 422 for a type mismatch; 400 is also defensible.
    ASSERT_STATUS_CODE_IN(response, (std::vector<int>{400, 422}));
}

// ---------------------------------------------------------------------------
// Creating users — the validation rules the service documents
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(api,
                    create_user_succeeds,
                    TestOptions{}
                        .describe("A well-formed registration returns 201 and the new id.")
                        .withTags({"api", "regression", "write"})) {
    ApiClient api = connect(ctx);

    json::Value payload = json::Value::object();
    // Unique username per run: the service enforces uniqueness, and a fixed
    // value would make this test pass once and fail forever after.
    //
    // example.com rather than example.invalid: RFC 2606 reserves both, but
    // email-validator (behind pydantic's EmailStr) rejects .invalid as a
    // special-use name, so the service would answer 422 for a reason that has
    // nothing to do with what this test is checking.
    payload.set("username", "tf_" + ids::generateHexId(6));
    payload.set("email", "tf_" + ids::generateHexId(6) + "@example.com");
    payload.set("password", "correct-horse-battery");

    const net::HttpResponse response = api.post("/users", payload);
    ASSERT_STATUS_CODE(response, 201);

    const json::Value body = response.jsonOrThrow();
    ASSERT_JSON_FIELD(body, "id");
    ASSERT_JSON_EQ(body, "username", payload.at("username"));
    ASSERT_FALSE(body.contains("password"));

    ctx.addMetadata("created_user_id", body.at("id").intOr(0));

    // Clean up so repeated runs do not grow the store without bound.
    const net::HttpResponse deleted = api.del("/users/" + std::to_string(body.at("id").intOr(0)));
    ASSERT_STATUS_CODE_IN(deleted, (std::vector<int>{200, 204}));
}

TESTFORGE_TEST_OPTS(api,
                    create_user_requires_email,
                    TestOptions{}
                        .describe("Email is required; omitting it is rejected.")
                        .withTags({"api", "negative", "validation"})) {
    ApiClient api = connect(ctx);

    json::Value payload = json::Value::object();
    payload.set("username", "tf_" + ids::generateHexId(6));
    payload.set("password", "correct-horse-battery");

    const net::HttpResponse response = api.post("/users", payload);
    ASSERT_STATUS_CODE_IN(response, (std::vector<int>{400, 422}));
}

TESTFORGE_TEST_OPTS(api,
                    create_user_rejects_short_password,
                    TestOptions{}
                        .describe("Passwords under 8 characters are rejected (boundary).")
                        .withTags({"api", "negative", "validation", "boundary"})) {
    ApiClient api = connect(ctx);

    json::Value payload = json::Value::object();
    payload.set("username", "tf_" + ids::generateHexId(6));
    payload.set("email", "tf_" + ids::generateHexId(6) + "@example.com");
    payload.set("password", "1234567");  // exactly one short of the minimum

    const net::HttpResponse response = api.post("/users", payload);
    ASSERT_STATUS_CODE_IN(response, (std::vector<int>{400, 422}));
}

TESTFORGE_TEST_OPTS(api,
                    create_user_accepts_minimum_password,
                    TestOptions{}
                        .describe("Exactly 8 characters is accepted (the other side of the "
                                  "boundary).")
                        .withTags({"api", "validation", "boundary", "write"})) {
    ApiClient api = connect(ctx);

    json::Value payload = json::Value::object();
    payload.set("username", "tf_" + ids::generateHexId(6));
    payload.set("email", "tf_" + ids::generateHexId(6) + "@example.com");
    payload.set("password", "12345678");

    const net::HttpResponse response = api.post("/users", payload);
    ASSERT_STATUS_CODE(response, 201);

    const json::Value body = response.jsonOrThrow();
    api.del("/users/" + std::to_string(body.at("id").intOr(0)));
}

TESTFORGE_TEST_OPTS(api,
                    create_user_rejects_duplicate_username,
                    TestOptions{}
                        .describe("Usernames are unique; a duplicate conflicts.")
                        .withTags({"api", "negative", "validation", "write"})) {
    ApiClient api = connect(ctx);

    json::Value payload = json::Value::object();
    const std::string username = "tf_dup_" + ids::generateHexId(6);
    payload.set("username", username);
    payload.set("email", "tf_" + ids::generateHexId(6) + "@example.com");
    payload.set("password", "correct-horse-battery");

    const net::HttpResponse first = api.post("/users", payload);
    ASSERT_STATUS_CODE(first, 201);
    const std::int64_t createdId = first.jsonOrThrow().at("id").intOr(0);

    // Same username, different email: only the username should conflict.
    payload.set("email", "tf_" + ids::generateHexId(6) + "@example.com");
    const net::HttpResponse second = api.post("/users", payload);
    ASSERT_STATUS_CODE(second, 409);

    api.del("/users/" + std::to_string(createdId));
}

TESTFORGE_TEST_OPTS(api,
                    malformed_json_is_rejected,
                    TestOptions{}
                        .describe("A body that is not JSON produces a 4xx, never a 5xx.")
                        .withTags({"api", "negative", "robustness"})) {
    ApiClient api = connect(ctx);

    const net::HttpResponse response =
        api.postRaw("/users", "{ this is not json", "application/json");
    // The distinction that matters: malformed input is the client's fault, so
    // a 5xx here would be a real defect.
    ASSERT_GE(response.statusCode, 400);
    ASSERT_LT(response.statusCode, 500);
}

// ---------------------------------------------------------------------------
// Updating and deleting
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(api,
                    update_user_lifecycle,
                    TestOptions{}
                        .describe("Create, update, verify and delete a user end to end.")
                        .withTags({"api", "regression", "write", "lifecycle"})
                        .withTimeout(20000)) {
    ApiClient api = connect(ctx);

    json::Value payload = json::Value::object();
    payload.set("username", "tf_life_" + ids::generateHexId(6));
    payload.set("email", "tf_" + ids::generateHexId(6) + "@example.com");
    payload.set("password", "correct-horse-battery");

    const net::HttpResponse created = api.post("/users", payload);
    ASSERT_STATUS_CODE(created, 201);
    const std::int64_t id = created.jsonOrThrow().at("id").intOr(0);
    ASSERT_GT(id, 0);

    json::Value update = json::Value::object();
    const std::string newEmail = "tf_updated_" + ids::generateHexId(6) + "@example.com";
    update.set("email", newEmail);

    const net::HttpResponse updated = api.put("/users/" + std::to_string(id), update);
    ASSERT_STATUS_CODE(updated, 200);
    ASSERT_JSON_EQ(updated.jsonOrThrow(), "email", newEmail);

    const net::HttpResponse fetched = api.get("/users/" + std::to_string(id));
    ASSERT_STATUS_CODE(fetched, 200);
    ASSERT_JSON_EQ(fetched.jsonOrThrow(), "email", newEmail);

    const net::HttpResponse deleted = api.del("/users/" + std::to_string(id));
    ASSERT_STATUS_CODE_IN(deleted, (std::vector<int>{200, 204}));

    const net::HttpResponse gone = api.get("/users/" + std::to_string(id));
    ASSERT_STATUS_CODE(gone, 404);
}

TESTFORGE_TEST_OPTS(api,
                    delete_unknown_user_returns_404,
                    TestOptions{}
                        .describe("Deleting something that does not exist is a 404.")
                        .withTags({"api", "negative"})) {
    ApiClient api = connect(ctx);
    const net::HttpResponse response = api.del("/users/999999");
    ASSERT_STATUS_CODE(response, 404);
}

// ---------------------------------------------------------------------------
// Items, pagination and auth placeholders
// ---------------------------------------------------------------------------

TESTFORGE_TEST_OPTS(api,
                    items_pagination,
                    TestOptions{}
                        .describe("limit and offset behave, and limit is bounded.")
                        .withTags({"api", "regression", "boundary"})) {
    ApiClient api = connect(ctx);

    const net::HttpResponse page = api.get("/items?limit=2&offset=0");
    ASSERT_STATUS_CODE(page, 200);
    const json::Value body = page.jsonOrThrow();
    ASSERT_JSON_FIELD(body, "items");
    ASSERT_LE(body.at("items").size(), std::size_t{2});

    // An absurd limit must be clamped or rejected, never honoured.
    const net::HttpResponse huge = api.get("/items?limit=100000");
    ASSERT_STATUS_CODE_IN(huge, (std::vector<int>{200, 400, 422}));
    if (huge.statusCode == 200) {
        ASSERT_LE(huge.jsonOrThrow().at("items").size(), std::size_t{1000});
    }

    const net::HttpResponse negative = api.get("/items?limit=-1");
    ASSERT_STATUS_CODE_IN(negative, (std::vector<int>{400, 422}));
}

TESTFORGE_TEST_OPTS(api,
                    protected_endpoint_requires_token,
                    TestOptions{}
                        .describe("The protected endpoint rejects an anonymous request.")
                        .withTags({"api", "negative", "auth"})) {
    ApiClient api = connect(ctx);

    const net::HttpResponse anonymous = api.get("/protected");
    ASSERT_STATUS_CODE_IN(anonymous, (std::vector<int>{401, 403}));

    net::HttpHeaders headers;
    headers.set("X-Api-Token", "wrong-token");
    const net::HttpResponse wrong = api.get("/protected", headers);
    ASSERT_STATUS_CODE_IN(wrong, (std::vector<int>{401, 403}));
}

TESTFORGE_TEST_OPTS(api,
                    slow_endpoint_meets_its_budget,
                    TestOptions{}
                        .describe("The deliberately slow endpoint still answers inside 3s.")
                        .withTags({"api", "performance"})
                        .withTimeout(15000)) {
    ApiClient api = connect(ctx);

    const net::HttpResponse response = api.get("/slow?delay_ms=250");
    ASSERT_STATUS_CODE(response, 200);
    ASSERT_RESPONSE_TIME(response, 3000);
    ctx.addMetadata("observed_latency_ms", millisOf(response.elapsed));
}
