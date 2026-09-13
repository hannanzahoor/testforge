"""Pytest suite for the sample service.

These are the *service's own* tests, separate from the TestForge suite that
also exercises it. Both exist on purpose: this file checks the service against
its contract using the framework its authors would reach for, and TestForge's
api suite checks the same service from the outside, over real HTTP. When the
two disagree, the disagreement is itself informative.

Run with::

    pytest sample-service/tests -q
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app.main import app


@pytest.fixture()
def client() -> TestClient:
    with TestClient(app) as test_client:
        # Every test starts from the seeded state, so ordering between tests
        # cannot matter.
        test_client.post("/reset")
        yield test_client


def unique(prefix: str = "u") -> str:
    import uuid

    return f"{prefix}_{uuid.uuid4().hex[:8]}"


# ---------------------------------------------------------------------------
# Health
# ---------------------------------------------------------------------------


def test_health_reports_ok(client: TestClient) -> None:
    response = client.get("/health")
    assert response.status_code == 200
    body = response.json()
    assert body["status"] == "ok"
    assert body["service"] == "testforge-sample-service"
    assert body["users"] >= 3


def test_response_carries_server_timing(client: TestClient) -> None:
    response = client.get("/health")
    assert "X-Server-Time-Ms" in response.headers
    assert float(response.headers["X-Server-Time-Ms"]) >= 0.0


# ---------------------------------------------------------------------------
# Users
# ---------------------------------------------------------------------------


def test_list_users_is_paginated(client: TestClient) -> None:
    response = client.get("/users", params={"limit": 2, "offset": 0})
    assert response.status_code == 200
    body = response.json()
    assert len(body["items"]) <= 2
    assert body["total"] >= 3
    assert body["limit"] == 2


def test_get_user_hides_the_password_hash(client: TestClient) -> None:
    response = client.get("/users/1")
    assert response.status_code == 200
    body = response.json()
    # The single most important assertion in this file.
    assert "password" not in body
    assert "password_hash" not in body


def test_unknown_user_is_404(client: TestClient) -> None:
    response = client.get("/users/999999")
    assert response.status_code == 404
    assert "detail" in response.json()


def test_non_numeric_id_is_rejected(client: TestClient) -> None:
    response = client.get("/users/not-a-number")
    assert response.status_code == 422


def test_create_user_returns_201(client: TestClient) -> None:
    username = unique("tf")
    response = client.post(
        "/users",
        json={
            "username": username,
            "email": f"{username}@example.com",
            "password": "correct-horse-battery",
        },
    )
    assert response.status_code == 201
    body = response.json()
    assert body["username"] == username
    assert "id" in body
    assert "password" not in body


@pytest.mark.parametrize(
    ("password", "expected"),
    [
        ("1234567", 422),  # one below the documented minimum
        ("12345678", 201),  # exactly the minimum
    ],
)
def test_password_length_boundary(client: TestClient, password: str, expected: int) -> None:
    username = unique("pw")
    response = client.post(
        "/users",
        json={
            "username": username,
            "email": f"{username}@example.com",
            "password": password,
        },
    )
    assert response.status_code == expected


def test_email_is_required(client: TestClient) -> None:
    username = unique("noemail")
    response = client.post(
        "/users", json={"username": username, "password": "correct-horse-battery"}
    )
    assert response.status_code == 422


def test_duplicate_username_conflicts(client: TestClient) -> None:
    username = unique("dup")
    payload = {
        "username": username,
        "email": f"{username}@example.com",
        "password": "correct-horse-battery",
    }
    assert client.post("/users", json=payload).status_code == 201

    payload["email"] = f"{unique('other')}@example.com"
    conflict = client.post("/users", json=payload)
    assert conflict.status_code == 409


def test_duplicate_email_conflicts(client: TestClient) -> None:
    email = f"{unique('shared')}@example.com"
    assert (
        client.post(
            "/users",
            json={"username": unique("a"), "email": email, "password": "correct-horse-battery"},
        ).status_code
        == 201
    )
    conflict = client.post(
        "/users",
        json={"username": unique("b"), "email": email, "password": "correct-horse-battery"},
    )
    assert conflict.status_code == 409


def test_username_character_rule(client: TestClient) -> None:
    response = client.post(
        "/users",
        json={
            "username": "not valid!",
            "email": f"{unique('x')}@example.com",
            "password": "correct-horse-battery",
        },
    )
    assert response.status_code == 422


def test_malformed_body_is_client_error(client: TestClient) -> None:
    response = client.post(
        "/users", content="{ not json", headers={"Content-Type": "application/json"}
    )
    # The distinction that matters: bad input is the caller's fault.
    assert 400 <= response.status_code < 500


def test_update_and_delete_lifecycle(client: TestClient) -> None:
    username = unique("life")
    created = client.post(
        "/users",
        json={
            "username": username,
            "email": f"{username}@example.com",
            "password": "correct-horse-battery",
        },
    )
    assert created.status_code == 201
    user_id = created.json()["id"]

    new_email = f"{unique('updated')}@example.com"
    updated = client.put(f"/users/{user_id}", json={"email": new_email})
    assert updated.status_code == 200
    assert updated.json()["email"] == new_email

    assert client.get(f"/users/{user_id}").json()["email"] == new_email

    assert client.delete(f"/users/{user_id}").status_code == 204
    assert client.get(f"/users/{user_id}").status_code == 404


def test_delete_unknown_user_is_404(client: TestClient) -> None:
    assert client.delete("/users/999999").status_code == 404


# ---------------------------------------------------------------------------
# Items
# ---------------------------------------------------------------------------


def test_items_respect_limit(client: TestClient) -> None:
    response = client.get("/items", params={"limit": 5})
    assert response.status_code == 200
    assert len(response.json()["items"]) == 5


@pytest.mark.parametrize("limit", [0, -1, 101, 100000])
def test_items_reject_out_of_range_limit(client: TestClient, limit: int) -> None:
    assert client.get("/items", params={"limit": limit}).status_code == 422


def test_items_filter_by_stock(client: TestClient) -> None:
    response = client.get("/items", params={"in_stock": "false", "limit": 100})
    assert response.status_code == 200
    assert all(item["in_stock"] is False for item in response.json()["items"])


# ---------------------------------------------------------------------------
# Fault endpoints — the behaviour TestForge's failure suite relies on
# ---------------------------------------------------------------------------


def test_boom_always_returns_500(client: TestClient) -> None:
    response = client.get("/boom")
    assert response.status_code == 500
    assert "detail" in response.json()


def test_slow_endpoint_honours_the_delay(client: TestClient) -> None:
    response = client.get("/slow", params={"delay_ms": 50})
    assert response.status_code == 200
    assert response.json()["slept_ms"] == 50


def test_protected_requires_a_token(client: TestClient) -> None:
    assert client.get("/protected").status_code == 401
    assert client.get("/protected", headers={"X-Api-Token": "wrong"}).status_code == 403
    ok = client.get("/protected", headers={"X-Api-Token": "testforge-demo-token"})
    assert ok.status_code == 200


def test_flaky_can_be_pinned_to_always_succeed(client: TestClient) -> None:
    # failure_rate=0 makes it deterministic, which is what lets this test exist
    # at all without being flaky itself.
    for _ in range(5):
        assert client.get("/flaky", params={"failure_rate": 0.0}).status_code == 200


def test_flaky_can_be_pinned_to_always_fail(client: TestClient) -> None:
    assert client.get("/flaky", params={"failure_rate": 1.0}).status_code == 503
