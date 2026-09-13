"""Fixed evaluation cases for AI test generation.

Each case pairs a natural-language requirement with the behaviours a competent
engineer would expect to see tested. The behaviours are matched by keyword,
which is a weak signal on purpose — it reliably catches "nothing in the output
mentions the uniqueness rule", and it deliberately does not claim to judge
whether a generated test is *correct*. That judgement needs execution, which is
what ``--run`` is for.

The set is small and stable. Its value is as a fixed yardstick across prompt
and model changes, not as a broad benchmark.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class ExpectedBehaviour:
    name: str
    keywords: list[str]


@dataclass(frozen=True)
class EvaluationCase:
    case_id: str
    suite: str
    requirement: str
    base_url: str = "http://127.0.0.1:8000"
    expected_behaviours: list[ExpectedBehaviour] = field(default_factory=list)


EVALUATION_CASES: list[EvaluationCase] = [
    EvaluationCase(
        case_id="user_registration",
        suite="eval_users",
        requirement=(
            "Create tests for a user registration API at POST /users. "
            "The username must be unique across all users. "
            "Email is required and must be a valid email address. "
            "The password must contain at least 8 characters. "
            "A successful registration returns 201 with the new user's id. "
            "GET /users/{id} returns a single user, or 404 if the id is unknown."
        ),
        expected_behaviours=[
            ExpectedBehaviour("successful registration", ["201"]),
            ExpectedBehaviour("duplicate username", ["duplicate", "unique", "409", "conflict"]),
            ExpectedBehaviour("missing email", ["email"]),
            ExpectedBehaviour("short password", ["password"]),
            ExpectedBehaviour("unknown user is 404", ["404"]),
            ExpectedBehaviour("reads a user by id", ["/users/"]),
        ],
    ),
    EvaluationCase(
        case_id="item_pagination",
        suite="eval_items",
        requirement=(
            "Write tests for GET /items, a paginated collection. "
            "It accepts limit (1-100, default 10) and offset (0 or greater). "
            "A limit outside the allowed range must be rejected. "
            "The response contains items, total, limit and offset."
        ),
        expected_behaviours=[
            ExpectedBehaviour("default page", ["/items"]),
            ExpectedBehaviour("explicit limit", ["limit"]),
            ExpectedBehaviour("offset", ["offset"]),
            ExpectedBehaviour("limit out of range", ["422", "400"]),
            ExpectedBehaviour("response shape", ["total", "items"]),
        ],
    ),
    EvaluationCase(
        case_id="health_and_auth",
        suite="eval_health",
        requirement=(
            "Test the service's operational endpoints. "
            "GET /health returns 200 with a status field set to 'ok'. "
            "GET /protected requires an X-Api-Token header; without it the "
            "service returns 401, and with an incorrect token it returns 403."
        ),
        expected_behaviours=[
            ExpectedBehaviour("health is 200", ["/health"]),
            ExpectedBehaviour("health status field", ["status"]),
            ExpectedBehaviour("anonymous is refused", ["401"]),
            ExpectedBehaviour("bad token is refused", ["403"]),
        ],
    ),
    EvaluationCase(
        case_id="error_handling",
        suite="eval_errors",
        requirement=(
            "Write negative tests for the users API. "
            "A malformed JSON body must produce a 4xx, never a 5xx. "
            "A non-numeric user id must be rejected. "
            "Deleting a user that does not exist returns 404. "
            "No endpoint may ever return a password or password hash."
        ),
        expected_behaviours=[
            ExpectedBehaviour("malformed body", ["400", "422"]),
            ExpectedBehaviour("bad id type", ["422", "400"]),
            ExpectedBehaviour("delete missing", ["delete", "404"]),
            ExpectedBehaviour("no password leak", ["password"]),
        ],
    ),
]
