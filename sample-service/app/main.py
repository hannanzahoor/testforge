"""Sample REST service — the system under test for TestForge's API suite.

This is a deliberately small FastAPI application with a deliberately
*interesting* set of behaviours. It is not a demonstration of how to write a
production service; it is a target that exercises the things an API test suite
needs to be able to check:

  * happy paths that return well-shaped JSON
  * validation with documented rules (unique username, required email,
    minimum password length) so boundary tests have a boundary to sit on
  * 404 for missing resources, 409 for conflicts, 422 for type errors
  * an endpoint that always fails (``/boom``), so the failure-injection suite
    can prove that a 5xx is classified as an application failure
  * an endpoint with controllable latency (``/slow``), for timeout tests
  * a token-protected endpoint, for authentication tests
  * an endpoint that fails intermittently (``/flaky``), for flake detection

State is in memory and resets when the process restarts. That is intentional:
a test target should be cheap to reset, and nothing here is worth persisting.

Run it with::

    uvicorn app.main:app --port 8000 --app-dir sample-service
"""

from __future__ import annotations

import asyncio
import hashlib
import itertools
import os
import random
import time
from datetime import datetime, timezone
from typing import Any

from fastapi import FastAPI, Header, HTTPException, Query, Request, Response
from fastapi.responses import JSONResponse
from pydantic import BaseModel, EmailStr, Field, field_validator

SERVICE_NAME = "testforge-sample-service"
SERVICE_VERSION = "1.0.0"

# The token the /protected endpoint expects. Overridable so the value in a test
# run is not the value in this file.
API_TOKEN = os.environ.get("SAMPLE_API_TOKEN", "testforge-demo-token")

# Password rule, stated once and enforced in one place so the API docs, the
# error message and the validator cannot disagree.
MIN_PASSWORD_LENGTH = 8

app = FastAPI(
    title="TestForge Sample Service",
    version=SERVICE_VERSION,
    description=(
        "A small, intentionally testable REST API. Used as the system under "
        "test for TestForge's API suite."
    ),
)


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------


class UserCreate(BaseModel):
    username: str = Field(min_length=3, max_length=32)
    email: EmailStr
    password: str = Field(min_length=MIN_PASSWORD_LENGTH, max_length=128)
    full_name: str | None = Field(default=None, max_length=120)

    @field_validator("username")
    @classmethod
    def username_is_simple(cls, value: str) -> str:
        # Restrictive on purpose: it gives the test suite an unambiguous
        # boundary, and it keeps usernames safe to put in a URL.
        if not all(c.isalnum() or c in "_-." for c in value):
            raise ValueError("username may contain only letters, digits, '_', '-' and '.'")
        return value


class UserUpdate(BaseModel):
    email: EmailStr | None = None
    full_name: str | None = Field(default=None, max_length=120)
    password: str | None = Field(default=None, min_length=MIN_PASSWORD_LENGTH, max_length=128)


class UserOut(BaseModel):
    id: int
    username: str
    email: str
    full_name: str | None = None
    created_at: str

    # Note what is absent: no password, no hash. A test asserts on that.


class UserPage(BaseModel):
    items: list[UserOut]
    total: int
    limit: int
    offset: int


class ItemOut(BaseModel):
    id: int
    name: str
    price_cents: int
    in_stock: bool


class ItemPage(BaseModel):
    items: list[ItemOut]
    total: int
    limit: int
    offset: int


# ---------------------------------------------------------------------------
# In-memory store
# ---------------------------------------------------------------------------


class Store:
    """Trivial in-memory store.

    A dict behind an asyncio lock rather than a database: the point of this
    service is to be a predictable target, and a real datastore would add a
    dependency and a failure mode that has nothing to do with what is being
    demonstrated.
    """

    def __init__(self) -> None:
        self._users: dict[int, dict[str, Any]] = {}
        self._ids = itertools.count(1)
        self._lock = asyncio.Lock()
        self._request_count = 0
        self._started = time.monotonic()
        self._seed()

    def _seed(self) -> None:
        for username, email, full_name in (
            ("alice", "alice@example.com", "Alice Example"),
            ("bob", "bob@example.com", "Bob Example"),
            ("carol", "carol@example.com", None),
        ):
            user_id = next(self._ids)
            self._users[user_id] = {
                "id": user_id,
                "username": username,
                "email": email,
                "full_name": full_name,
                # Stored hashed even though nothing reads it, so the shape of
                # the record is realistic. Not a password-hashing scheme worth
                # copying: use argon2/bcrypt in anything real.
                "password_hash": hashlib.sha256(b"seeded-password").hexdigest(),
                "created_at": datetime.now(timezone.utc).isoformat(),
            }

    @property
    def lock(self) -> asyncio.Lock:
        return self._lock

    def next_id(self) -> int:
        return next(self._ids)

    @property
    def users(self) -> dict[int, dict[str, Any]]:
        return self._users

    def find_by_username(self, username: str) -> dict[str, Any] | None:
        lowered = username.lower()
        return next((u for u in self._users.values() if u["username"].lower() == lowered), None)

    def find_by_email(self, email: str) -> dict[str, Any] | None:
        lowered = email.lower()
        return next((u for u in self._users.values() if u["email"].lower() == lowered), None)

    def note_request(self) -> int:
        self._request_count += 1
        return self._request_count

    @property
    def request_count(self) -> int:
        return self._request_count

    @property
    def uptime_seconds(self) -> float:
        return time.monotonic() - self._started


store = Store()

ITEMS: list[dict[str, Any]] = [
    {"id": i, "name": f"item-{i:03d}", "price_cents": 100 * i, "in_stock": i % 4 != 0}
    for i in range(1, 51)
]


def to_public(user: dict[str, Any]) -> UserOut:
    """Projects a stored record onto the public shape.

    Explicit field-by-field rather than ``del user["password_hash"]``: a new
    sensitive field added to the store should not leak by default.
    """
    return UserOut(
        id=user["id"],
        username=user["username"],
        email=user["email"],
        full_name=user.get("full_name"),
        created_at=user["created_at"],
    )


# ---------------------------------------------------------------------------
# Middleware
# ---------------------------------------------------------------------------


@app.middleware("http")
async def add_request_metadata(request: Request, call_next):  # type: ignore[no-untyped-def]
    """Counts requests and stamps a server-side duration header.

    The header gives API tests something to assert on that the client cannot
    fake, which is useful when investigating whether latency is in the service
    or on the wire.
    """
    started = time.perf_counter()
    count = store.note_request()
    response = await call_next(request)
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    response.headers["X-Server-Time-Ms"] = f"{elapsed_ms:.2f}"
    response.headers["X-Request-Number"] = str(count)
    return response


# ---------------------------------------------------------------------------
# Health
# ---------------------------------------------------------------------------


@app.get("/health", tags=["health"])
async def health() -> dict[str, Any]:
    return {
        "status": "ok",
        "service": SERVICE_NAME,
        "version": SERVICE_VERSION,
        "uptime_seconds": round(store.uptime_seconds, 3),
        "users": len(store.users),
        "requests_served": store.request_count,
    }


@app.get("/ready", tags=["health"])
async def ready() -> dict[str, str]:
    return {"status": "ready"}


# ---------------------------------------------------------------------------
# Users
# ---------------------------------------------------------------------------


@app.get("/users", response_model=UserPage, tags=["users"])
async def list_users(
    limit: int = Query(default=20, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    username: str | None = Query(default=None, max_length=32),
) -> UserPage:
    records = sorted(store.users.values(), key=lambda u: u["id"])
    if username:
        needle = username.lower()
        records = [u for u in records if needle in u["username"].lower()]

    window = records[offset : offset + limit]
    return UserPage(
        items=[to_public(u) for u in window],
        total=len(records),
        limit=limit,
        offset=offset,
    )


@app.get("/users/{user_id}", response_model=UserOut, tags=["users"])
async def get_user(user_id: int) -> UserOut:
    user = store.users.get(user_id)
    if user is None:
        raise HTTPException(status_code=404, detail=f"no user with id {user_id}")
    return to_public(user)


@app.post("/users", response_model=UserOut, status_code=201, tags=["users"])
async def create_user(payload: UserCreate) -> UserOut:
    async with store.lock:
        # Uniqueness is checked under the lock so two concurrent creates cannot
        # both pass the check. The API test suite creates users in parallel.
        if store.find_by_username(payload.username) is not None:
            raise HTTPException(
                status_code=409, detail=f"username '{payload.username}' is already taken"
            )
        if store.find_by_email(payload.email) is not None:
            raise HTTPException(
                status_code=409, detail=f"email '{payload.email}' is already registered"
            )

        user_id = store.next_id()
        record = {
            "id": user_id,
            "username": payload.username,
            "email": payload.email,
            "full_name": payload.full_name,
            "password_hash": hashlib.sha256(payload.password.encode("utf-8")).hexdigest(),
            "created_at": datetime.now(timezone.utc).isoformat(),
        }
        store.users[user_id] = record

    return to_public(record)


@app.put("/users/{user_id}", response_model=UserOut, tags=["users"])
async def update_user(user_id: int, payload: UserUpdate) -> UserOut:
    async with store.lock:
        user = store.users.get(user_id)
        if user is None:
            raise HTTPException(status_code=404, detail=f"no user with id {user_id}")

        if payload.email is not None:
            existing = store.find_by_email(payload.email)
            if existing is not None and existing["id"] != user_id:
                raise HTTPException(
                    status_code=409, detail=f"email '{payload.email}' is already registered"
                )
            user["email"] = payload.email

        if payload.full_name is not None:
            user["full_name"] = payload.full_name
        if payload.password is not None:
            user["password_hash"] = hashlib.sha256(payload.password.encode("utf-8")).hexdigest()

    return to_public(user)


@app.delete("/users/{user_id}", status_code=204, response_class=Response, tags=["users"])
async def delete_user(user_id: int) -> Response:
    # response_class=Response and an explicit empty Response: FastAPI refuses
    # to attach a body-producing response model to a 204, and RFC 9110 says a
    # 204 carries no body.
    async with store.lock:
        if store.users.pop(user_id, None) is None:
            raise HTTPException(status_code=404, detail=f"no user with id {user_id}")
    return Response(status_code=204)


# ---------------------------------------------------------------------------
# Items — a read-only collection, for pagination tests
# ---------------------------------------------------------------------------


@app.get("/items", response_model=ItemPage, tags=["items"])
async def list_items(
    limit: int = Query(default=10, ge=1, le=100),
    offset: int = Query(default=0, ge=0),
    in_stock: bool | None = Query(default=None),
) -> ItemPage:
    records = ITEMS
    if in_stock is not None:
        records = [i for i in records if i["in_stock"] is in_stock]

    window = records[offset : offset + limit]
    return ItemPage(
        items=[ItemOut(**i) for i in window],
        total=len(records),
        limit=limit,
        offset=offset,
    )


@app.get("/items/{item_id}", response_model=ItemOut, tags=["items"])
async def get_item(item_id: int) -> ItemOut:
    item = next((i for i in ITEMS if i["id"] == item_id), None)
    if item is None:
        raise HTTPException(status_code=404, detail=f"no item with id {item_id}")
    return ItemOut(**item)


# ---------------------------------------------------------------------------
# Endpoints that exist purely so tests have something to catch
# ---------------------------------------------------------------------------


@app.get("/boom", tags=["faults"])
async def boom() -> JSONResponse:
    """Always fails with a 500.

    This is what proves the failure classifier works: a test asserting 200
    against this endpoint fails, and TestForge should record it as
    APPLICATION_FAILURE rather than blaming the assertion.
    """
    raise HTTPException(
        status_code=500,
        detail="deliberate server-side failure for TestForge's failure-injection suite",
    )


@app.get("/slow", tags=["faults"])
async def slow(delay_ms: int = Query(default=1000, ge=0, le=30000)) -> dict[str, Any]:
    """Sleeps for the requested time, then answers. For latency and timeout tests."""
    await asyncio.sleep(delay_ms / 1000.0)
    return {"slept_ms": delay_ms, "status": "ok"}


@app.get("/flaky", tags=["faults"])
async def flaky(failure_rate: float = Query(default=0.5, ge=0.0, le=1.0)) -> dict[str, Any]:
    """Fails a configurable fraction of the time.

    Genuinely random, so the flake-detection heuristic has something real to
    look at rather than a fixed pattern it could accidentally learn.
    """
    if random.random() < failure_rate:
        raise HTTPException(status_code=503, detail="flaky endpoint chose to fail this time")
    return {"status": "ok", "lucky": True}


@app.get("/protected", tags=["auth"])
async def protected(x_api_token: str | None = Header(default=None)) -> dict[str, str]:
    """Requires a shared token in the X-Api-Token header.

    A placeholder for real authentication: enough for tests to check that an
    anonymous request is refused and a valid one is not, without pretending to
    be an identity system.
    """
    if x_api_token is None:
        raise HTTPException(status_code=401, detail="X-Api-Token header is required")
    if x_api_token != API_TOKEN:
        raise HTTPException(status_code=403, detail="invalid API token")
    return {"status": "ok", "message": "authenticated"}


@app.get("/echo", tags=["debug"])
async def echo(request: Request) -> dict[str, Any]:
    """Reflects the request back. Useful when debugging the HTTP client."""
    return {
        "method": request.method,
        "path": request.url.path,
        "query": dict(request.query_params),
        # Header values are echoed as-is; do not send anything secret here.
        "headers": {k.lower(): v for k, v in request.headers.items()},
        "client": request.client.host if request.client else None,
    }


@app.post("/reset", status_code=200, tags=["debug"])
async def reset() -> dict[str, Any]:
    """Restores the seeded state. Lets a suite start from a known point."""
    global store
    store = Store()
    return {"status": "reset", "users": len(store.users)}


# ---------------------------------------------------------------------------
# Error shape
# ---------------------------------------------------------------------------


@app.exception_handler(HTTPException)
async def http_exception_handler(request: Request, exc: HTTPException) -> JSONResponse:
    """One error shape for the whole API.

    Every error carries ``detail``, so a test can assert on the shape of a
    failure as well as its status code.
    """
    return JSONResponse(
        status_code=exc.status_code,
        content={
            "detail": exc.detail,
            "status": exc.status_code,
            "path": request.url.path,
        },
    )
