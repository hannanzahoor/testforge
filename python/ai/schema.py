"""Specification schema and validation for the AI sidecar.

This is the *first* of three checks a generated specification passes:

  1. here, in the sidecar, so a malformed generation is rejected before it
     crosses a process boundary and the model can be blamed precisely;
  2. in ``SpecValidator`` on the C++ side, which is the security boundary —
     methods, endpoints, headers and URL policy;
  3. again in ``registerSpecSuite``, immediately before registration, so a
     caller that skipped step 2 cannot get anything runnable.

Duplicating the check is deliberate. Step 1 gives a good error message; step 2
is the one that must be correct.

No jsonschema dependency: the shape is small and fixed, and hand-written checks
give better messages than a generic validator would.
"""

from __future__ import annotations

from typing import Any

# Advertised to the model in the prompt payload so it has the shape in front of
# it, and reused by the evaluation harness to score conformance.
SPEC_JSON_SCHEMA: dict[str, Any] = {
    "type": "object",
    "required": ["suite", "tests"],
    "properties": {
        "suite": {"type": "string"},
        "requirement": {"type": "string"},
        "tests": {
            "type": "array",
            "items": {
                "type": "object",
                "required": ["name", "method", "endpoint", "expected_status"],
                "properties": {
                    "name": {"type": "string"},
                    "description": {"type": "string"},
                    "method": {
                        "type": "string",
                        "enum": ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"],
                    },
                    "endpoint": {"type": "string", "pattern": "^/"},
                    "headers": {"type": "object"},
                    "body": {},
                    "expected_status": {"type": "integer", "minimum": 100, "maximum": 599},
                    "max_response_time_ms": {"type": "integer", "minimum": 0},
                    "assertions": {"type": "array"},
                    "tags": {"type": "array", "items": {"type": "string"}},
                },
            },
        },
    },
}

ALLOWED_METHODS = frozenset({"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"})

ALLOWED_ASSERTION_KINDS = frozenset(
    {
        "status_code",
        "status_code_in",
        "response_time_under_ms",
        "body_contains",
        "body_not_contains",
        "json_field_exists",
        "json_field_equals",
        "header_exists",
        "header_equals",
    }
)

# Headers a generated test may never set: credentials, and anything that
# changes where the request actually goes.
FORBIDDEN_HEADERS = frozenset(
    {
        "authorization",
        "cookie",
        "set-cookie",
        "proxy-authorization",
        "host",
        "x-forwarded-for",
        "x-forwarded-host",
        "x-real-ip",
        "content-length",
        "transfer-encoding",
        "connection",
        "upgrade",
    }
)

NAME_CHARACTERS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.")


def _endpoint_problem(endpoint: Any) -> str:
    if not isinstance(endpoint, str) or not endpoint:
        return "endpoint must be a non-empty string"
    if not endpoint.startswith("/"):
        return "endpoint must be a relative path starting with '/'"
    if endpoint.startswith("//"):
        return "endpoint must not start with '//'"
    if ".." in endpoint:
        return "endpoint must not contain '..'"
    if "@" in endpoint:
        return "endpoint must not contain '@'"
    lowered = endpoint.lower()
    for scheme in ("http:", "https:", "file:", "ftp:", "data:", "javascript:"):
        if scheme in lowered:
            return f"endpoint must not contain the '{scheme}' scheme"
    if any(ord(c) < 0x20 or ord(c) == 0x7F for c in endpoint):
        return "endpoint must not contain control characters"
    return ""


def _name_problem(name: Any) -> str:
    if not isinstance(name, str) or not name:
        return "name must be a non-empty string"
    if len(name) > 80:
        return "name must be 80 characters or fewer"
    bad = sorted(set(name) - NAME_CHARACTERS)
    if bad:
        return f"name contains unsupported character(s): {''.join(bad)!r}"
    if name.startswith(".") or ".." in name:
        return "name must not start with '.' or contain '..'"
    return ""


def validate_test(test: Any, index: int) -> list[str]:
    """Returns the problems with one test. Empty means acceptable."""
    where = f"tests[{index}]"
    if not isinstance(test, dict):
        return [f"{where}: must be an object"]

    problems: list[str] = []

    if problem := _name_problem(test.get("name")):
        problems.append(f"{where}.name: {problem}")

    method = test.get("method", "GET")
    if not isinstance(method, str) or method.upper() not in ALLOWED_METHODS:
        problems.append(
            f"{where}.method: '{method}' is not allowed "
            f"(allowed: {', '.join(sorted(ALLOWED_METHODS))})"
        )

    if problem := _endpoint_problem(test.get("endpoint")):
        problems.append(f"{where}.endpoint: {problem}")

    status = test.get("expected_status", 200)
    if not isinstance(status, int) or isinstance(status, bool) or not 100 <= status <= 599:
        problems.append(f"{where}.expected_status: {status!r} is not a status code in 100-599")

    budget = test.get("max_response_time_ms")
    if budget is not None and (
        not isinstance(budget, int) or isinstance(budget, bool) or not 0 <= budget <= 600_000
    ):
        problems.append(f"{where}.max_response_time_ms: must be an integer between 0 and 600000")

    headers = test.get("headers", {})
    if headers:
        if not isinstance(headers, dict):
            problems.append(f"{where}.headers: must be an object")
        else:
            if len(headers) > 16:
                problems.append(f"{where}.headers: more than 16 headers")
            for key in headers:
                if str(key).lower() in FORBIDDEN_HEADERS:
                    problems.append(f"{where}.headers: '{key}' may not be set by a generated test")

    assertions = test.get("assertions", [])
    if assertions:
        if not isinstance(assertions, list):
            problems.append(f"{where}.assertions: must be an array")
        else:
            if len(assertions) > 20:
                problems.append(f"{where}.assertions: more than 20 assertions")
            for i, assertion in enumerate(assertions):
                spot = f"{where}.assertions[{i}]"
                if not isinstance(assertion, dict):
                    problems.append(f"{spot}: must be an object")
                    continue
                kind = assertion.get("kind")
                if kind not in ALLOWED_ASSERTION_KINDS:
                    problems.append(
                        f"{spot}.kind: '{kind}' is not a supported assertion "
                        f"(supported: {', '.join(sorted(ALLOWED_ASSERTION_KINDS))})"
                    )
                    continue
                needs_target = kind in {
                    "json_field_exists",
                    "json_field_equals",
                    "header_exists",
                    "header_equals",
                }
                if needs_target and not assertion.get("target"):
                    problems.append(f"{spot}.target: '{kind}' requires a target")

    if method.upper() in {"GET", "HEAD"} and test.get("body") not in (None, {}, ""):
        problems.append(f"{where}.body: a {method.upper()} request should not carry a body")

    return problems


def validate_specification(specification: Any, suite: str = "", max_tests: int = 50) -> list[str]:
    """Returns every problem found. An empty list means the spec is acceptable."""
    if not isinstance(specification, dict):
        return ["specification: must be a JSON object"]

    problems: list[str] = []

    declared_suite = specification.get("suite", suite)
    if problem := _name_problem(declared_suite):
        problems.append(f"suite: {problem}")

    tests = specification.get("tests")
    if not isinstance(tests, list):
        return [*problems, "tests: must be an array"]
    if not tests:
        return [*problems, "tests: the specification contains no tests"]
    if len(tests) > max_tests:
        problems.append(f"tests: {len(tests)} tests exceeds the requested maximum of {max_tests}")

    seen: dict[str, int] = {}
    for index, test in enumerate(tests):
        problems.extend(validate_test(test, index))
        if isinstance(test, dict):
            name = str(test.get("name", "")).lower()
            if name and name in seen:
                problems.append(f"tests[{index}].name: duplicates tests[{seen[name]}] ('{name}')")
            elif name:
                seen[name] = index

    return problems
