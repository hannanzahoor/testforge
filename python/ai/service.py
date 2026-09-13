"""TestForge AI sidecar.

The C++ engine never talks to a model provider directly. It talks to this
service, which owns the API key, the prompts, and the retry behaviour. The
reasons are set out in docs/ai-architecture.md; briefly:

  * the credential lives in one process, in the language whose SDK owns the
    provider's retry and rate-limit semantics;
  * the engine links no TLS stack, so it cannot reach an https endpoint;
  * prompts change far more often than the test engine does, and changing one
    should not mean recompiling C++.

Contract (both directions are validated on the C++ side as well):

    GET  /health              -> {"ok", "provider", "model", "key_present", ...}
    POST /generate-tests      <- {"requirement", "suite", "base_url", "max_tests"}
                              -> {"ok", "specification": {...}, "usage": {...}}
    POST /analyze-failure     <- {"context", "test", "failure_category"}
                              -> {"ok", "analysis": {...}}

Run it with::

    uvicorn python.ai.service:app --port 8810

With no OPENAI_API_KEY set the service still starts and still answers, in
"unavailable" mode: it reports ok=false with a reason. It never fabricates a
response and calls it a model output — a fabricated analysis is worse than no
analysis, because someone will act on it.
"""

from __future__ import annotations

import json
import logging
import os
import time
from typing import Any

from fastapi import FastAPI
from pydantic import BaseModel, Field

from . import prompts
from .schema import SPEC_JSON_SCHEMA, validate_specification

LOG = logging.getLogger("testforge.ai")
logging.basicConfig(level=os.environ.get("TESTFORGE_AI_LOG_LEVEL", "INFO"))

SERVICE_VERSION = "1.0.0"
DEFAULT_MODEL = os.environ.get("TESTFORGE_AI_MODEL", "gpt-4o-mini")

# Hard caps applied before anything reaches the provider. They bound both cost
# and blast radius: a runaway loop in a caller cannot turn into a large bill.
MAX_REQUIREMENT_CHARS = 8_000
MAX_CONTEXT_CHARS = 32_000
MAX_TESTS_CEILING = 50
REQUEST_TIMEOUT_SECONDS = float(os.environ.get("TESTFORGE_AI_TIMEOUT_S", "60"))

app = FastAPI(
    title="TestForge AI Sidecar",
    version=SERVICE_VERSION,
    description="Model orchestration for TestForge. Advisory output only.",
)


# ---------------------------------------------------------------------------
# Request/response models
# ---------------------------------------------------------------------------


class GenerateRequest(BaseModel):
    requirement: str = Field(max_length=MAX_REQUIREMENT_CHARS)
    suite: str = Field(default="generated", max_length=64)
    base_url: str = Field(default="", max_length=512)
    max_tests: int = Field(default=10, ge=1, le=MAX_TESTS_CEILING)
    model: str | None = Field(default=None, max_length=64)
    context: dict[str, Any] = Field(default_factory=dict)


class AnalyzeRequest(BaseModel):
    context: dict[str, Any] = Field(default_factory=dict)
    test: str = Field(default="", max_length=256)
    failure_category: str = Field(default="", max_length=64)
    model: str | None = Field(default=None, max_length=64)


# ---------------------------------------------------------------------------
# Provider
# ---------------------------------------------------------------------------


class ModelUnavailable(RuntimeError):
    """Raised when no model can be reached. Never swallowed into a fake answer."""


def _api_key() -> str | None:
    key = os.environ.get("OPENAI_API_KEY", "").strip()
    return key or None


def _client() -> Any:
    """Builds an OpenAI client, or explains precisely why it cannot."""
    key = _api_key()
    if key is None:
        raise ModelUnavailable(
            "OPENAI_API_KEY is not set. Export it, or run TestForge with "
            "TESTFORGE_AI_MOCK=1 for the offline deterministic provider."
        )
    try:
        from openai import OpenAI  # imported lazily: the package is optional
    except ImportError as exc:  # pragma: no cover - depends on the environment
        raise ModelUnavailable(
            "the 'openai' package is not installed (pip install -r python/requirements.txt)"
        ) from exc

    base_url = os.environ.get("OPENAI_BASE_URL") or None
    return OpenAI(api_key=key, base_url=base_url, timeout=REQUEST_TIMEOUT_SECONDS)


def _call_model(
    system_prompt: str, user_payload: dict[str, Any], model: str
) -> tuple[dict[str, Any], dict[str, int]]:
    """One model call that must return a JSON object.

    ``response_format={"type": "json_object"}`` is what makes the output
    parseable rather than hopeful. It is still validated afterwards: structured
    output mode constrains the syntax, not the semantics, and the C++ side
    validates a third time before anything executes.
    """
    client = _client()

    response = client.chat.completions.create(
        model=model,
        response_format={"type": "json_object"},
        # Low but not zero. Deterministic-ish output makes runs comparable;
        # exactly 0 tends to collapse into the same handful of test ideas.
        temperature=0.2,
        messages=[
            {"role": "system", "content": system_prompt},
            # The caller's text goes inside a JSON envelope in a *user* message.
            # It is never concatenated into the system prompt, so instructions
            # hidden in a requirement or in a captured response body arrive as
            # data the system prompt has already told the model to distrust.
            {"role": "user", "content": json.dumps(user_payload, ensure_ascii=False)},
        ],
    )

    content = response.choices[0].message.content or "{}"
    usage = {
        "prompt_tokens": getattr(response.usage, "prompt_tokens", 0) or 0,
        "completion_tokens": getattr(response.usage, "completion_tokens", 0) or 0,
    }

    try:
        parsed = json.loads(content)
    except json.JSONDecodeError as exc:
        raise ModelUnavailable(f"the model returned text that is not valid JSON: {exc}") from exc
    if not isinstance(parsed, dict):
        raise ModelUnavailable("the model returned JSON that is not an object")
    return parsed, usage


def _truncate_context(context: dict[str, Any]) -> dict[str, Any]:
    """Second line of defence on size.

    The C++ FailureContext already applies budgets, but this service is
    reachable on its own and must not forward an unbounded document.
    """
    encoded = json.dumps(context, ensure_ascii=False)
    if len(encoded) <= MAX_CONTEXT_CHARS:
        return context
    LOG.warning("failure context truncated from %d characters", len(encoded))
    return {
        "truncated": True,
        "note": f"context exceeded {MAX_CONTEXT_CHARS} characters and was reduced",
        "result": context.get("result", {}),
        "test": context.get("test", {}),
    }


# ---------------------------------------------------------------------------
# Endpoints
# ---------------------------------------------------------------------------


@app.get("/health")
async def health() -> dict[str, Any]:
    key = _api_key()
    try:
        import openai  # noqa: F401

        sdk_present = True
    except ImportError:
        sdk_present = False

    return {
        "ok": True,
        "service": "testforge-ai-sidecar",
        "version": SERVICE_VERSION,
        "provider": "openai",
        "model": DEFAULT_MODEL,
        # Presence only. The key itself is never returned, logged, or echoed.
        "key_present": key is not None,
        "sdk_present": sdk_present,
        "ready": key is not None and sdk_present,
        "reason": (
            None
            if key is not None and sdk_present
            else "set OPENAI_API_KEY and install the openai package to enable live calls"
        ),
    }


@app.post("/generate-tests")
async def generate_tests(request: GenerateRequest) -> dict[str, Any]:
    started = time.perf_counter()
    model = request.model or DEFAULT_MODEL
    max_tests = min(request.max_tests, MAX_TESTS_CEILING)

    payload = {
        "requirement": request.requirement,
        "suite_name": request.suite,
        "max_tests": max_tests,
        "known_context": request.context,
        "output_schema": SPEC_JSON_SCHEMA,
    }

    try:
        raw, usage = _call_model(prompts.GENERATE_TESTS_SYSTEM, payload, model)
    except ModelUnavailable as exc:
        LOG.warning("generation unavailable: %s", exc)
        return {"ok": False, "error": str(exc), "model": model}
    except Exception as exc:
        LOG.exception("generation failed")
        return {"ok": False, "error": f"{type(exc).__name__}: {exc}", "model": model}

    specification = raw.get("specification", raw)
    problems = validate_specification(specification, suite=request.suite, max_tests=max_tests)
    if problems:
        # Report the schema failure rather than repairing it. A silently
        # "fixed" specification is one nobody reviewed.
        return {
            "ok": False,
            "error": "the model's output did not match the required schema",
            "problems": problems,
            "raw": specification,
            "model": model,
        }

    specification.setdefault("suite", request.suite)
    specification.setdefault("requirement", request.requirement)
    specification["source"] = f"ai:{model}"
    specification["model"] = model

    return {
        "ok": True,
        "model": model,
        "elapsed_ms": int((time.perf_counter() - started) * 1000),
        "specification": specification,
        "usage": usage,
    }


@app.post("/analyze-failure")
async def analyze_failure(request: AnalyzeRequest) -> dict[str, Any]:
    started = time.perf_counter()
    model = request.model or DEFAULT_MODEL

    payload = {
        "test": request.test,
        "engine_failure_category": request.failure_category,
        "evidence": _truncate_context(request.context),
    }

    try:
        raw, usage = _call_model(prompts.ANALYZE_FAILURE_SYSTEM, payload, model)
    except ModelUnavailable as exc:
        LOG.warning("analysis unavailable: %s", exc)
        return {"ok": False, "error": str(exc), "model": model}
    except Exception as exc:
        LOG.exception("analysis failed")
        return {"ok": False, "error": f"{type(exc).__name__}: {exc}", "model": model}

    analysis = raw.get("analysis", raw)
    if not isinstance(analysis, dict) or not analysis.get("probable_cause"):
        return {
            "ok": False,
            "error": "the model's analysis had no 'probable_cause'",
            "raw": raw,
            "model": model,
        }

    def _strings(key: str) -> list[str]:
        value = analysis.get(key, [])
        if isinstance(value, str):
            value = [value]
        if not isinstance(value, list):
            return []
        return [str(item)[:500] for item in value][:10]

    confidence = analysis.get("confidence", 0.0)
    try:
        confidence = max(0.0, min(1.0, float(confidence)))
    except (TypeError, ValueError):
        confidence = 0.0

    return {
        "ok": True,
        "model": model,
        "elapsed_ms": int((time.perf_counter() - started) * 1000),
        "usage": usage,
        "analysis": {
            "probable_cause": str(analysis["probable_cause"])[:2000],
            # The model's category is reported, never applied. The engine
            # decided the verdict before this call was made.
            "category": str(analysis.get("category", ""))[:64],
            "evidence": _strings("evidence"),
            "suggested_investigation": _strings("suggested_investigation"),
            "confidence": confidence,
        },
    }
