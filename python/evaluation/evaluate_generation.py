"""Evaluation harness for AI-generated test specifications.

A generated test suite is itself a piece of software output, and output that
nobody measures is output nobody can improve. This harness scores a batch of
generations against a fixed set of requirements and reports metrics that can be
tracked across prompt or model changes.

What it measures, and what each metric is worth:

  schema_validity      Fraction of generations that parsed and passed the
                       validator. The floor: a spec that fails here produces
                       zero tests, whatever else is good about it.

  test_validity        Fraction of individual tests that survived validation.
                       Distinguishes "the model produced 10 tests, 1 bad" from
                       "the model produced 1 test, and it was bad".

  duplicate_rate       Fraction of tests that duplicate another by
                       (method, endpoint, expected_status). High duplication
                       inflates the count without adding coverage.

  requirement_coverage Fraction of each requirement's expected behaviours that
                       some generated test appears to exercise, matched by
                       keyword. HEURISTIC — it detects "no test mentions the
                       password rule at all", not whether the test is correct.

  negative_ratio       Fraction of tests expecting a 4xx/5xx. A suite of only
                       happy paths tests very little.

  execution_pass_rate  Optional. With --run and a live service, the fraction of
                       generated tests whose expectation actually held. This is
                       the only metric here that reflects reality rather than
                       shape; it needs a service to run against, so it is off
                       by default.

Usage::

    python -m python.evaluation.evaluate_generation --mock
    python -m python.evaluation.evaluate_generation --endpoint http://127.0.0.1:8810
    python -m python.evaluation.evaluate_generation --mock --run --testforge ./build/testforge
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# Import the same validator the sidecar uses, so the score reflects the real
# gate rather than a second opinion.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from python.ai.schema import validate_specification
from python.evaluation.cases import EVALUATION_CASES, EvaluationCase


@dataclass
class GenerationOutcome:
    case_id: str
    ok: bool
    error: str = ""
    elapsed_ms: int = 0
    specification: dict[str, Any] = field(default_factory=dict)
    problems: list[str] = field(default_factory=list)


@dataclass
class Metrics:
    generations: int = 0
    schema_valid: int = 0
    tests_total: int = 0
    tests_valid: int = 0
    tests_duplicate: int = 0
    tests_negative: int = 0
    behaviours_expected: int = 0
    behaviours_covered: int = 0
    executed: int = 0
    executed_passed: int = 0
    total_elapsed_ms: int = 0

    def as_report(self) -> dict[str, Any]:
        def ratio(numerator: int, denominator: int) -> float | None:
            return round(numerator / denominator, 4) if denominator else None

        return {
            "generations": self.generations,
            "schema_validity": ratio(self.schema_valid, self.generations),
            "tests_generated": self.tests_total,
            "test_validity": ratio(self.tests_valid, self.tests_total),
            "duplicate_rate": ratio(self.tests_duplicate, self.tests_total),
            "negative_ratio": ratio(self.tests_negative, self.tests_total),
            "requirement_coverage": ratio(self.behaviours_covered, self.behaviours_expected),
            "execution_pass_rate": ratio(self.executed_passed, self.executed),
            "tests_executed": self.executed,
            "average_generation_ms": (
                round(self.total_elapsed_ms / self.generations) if self.generations else None
            ),
        }


# ---------------------------------------------------------------------------
# Generation
# ---------------------------------------------------------------------------


def generate_via_sidecar(
    case: EvaluationCase, endpoint: str, max_tests: int, timeout: float
) -> GenerationOutcome:
    import httpx

    started = time.perf_counter()
    try:
        response = httpx.post(
            endpoint.rstrip("/") + "/generate-tests",
            json={
                "requirement": case.requirement,
                "suite": case.suite,
                "base_url": case.base_url,
                "max_tests": max_tests,
            },
            timeout=timeout,
        )
    except Exception as exc:
        return GenerationOutcome(case.case_id, False, f"{type(exc).__name__}: {exc}")

    elapsed = int((time.perf_counter() - started) * 1000)
    if response.status_code != 200:
        return GenerationOutcome(
            case.case_id, False, f"HTTP {response.status_code}: {response.text[:300]}", elapsed
        )

    payload = response.json()
    if not payload.get("ok"):
        return GenerationOutcome(
            case.case_id, False, str(payload.get("error", "unknown error")), elapsed
        )
    return GenerationOutcome(case.case_id, True, "", elapsed, payload.get("specification", {}))


def generate_via_testforge_mock(
    case: EvaluationCase, testforge: str, max_tests: int
) -> GenerationOutcome:
    """Uses TestForge's own deterministic mock provider.

    Lets the harness run in CI with no API key: it exercises the whole
    validate-and-score path, and the numbers it produces are stable, so a
    change in the *harness* is visible even when no model is involved.
    """
    started = time.perf_counter()
    command = [
        testforge,
        "ai",
        "generate-tests",
        "--mock",
        "--json",
        "--suite",
        case.suite,
        "--max-tests",
        str(max_tests),
        "--requirement",
        case.requirement,
    ]
    try:
        completed = subprocess.run(
            command, capture_output=True, text=True, timeout=120, check=False
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return GenerationOutcome(case.case_id, False, f"{type(exc).__name__}: {exc}")

    elapsed = int((time.perf_counter() - started) * 1000)
    if not completed.stdout.strip():
        return GenerationOutcome(
            case.case_id, False, completed.stderr.strip()[:300] or "no output", elapsed
        )
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        return GenerationOutcome(case.case_id, False, f"unparseable output: {exc}", elapsed)

    if not payload.get("ok"):
        return GenerationOutcome(
            case.case_id, False, str(payload.get("error", "generation failed")), elapsed
        )
    return GenerationOutcome(case.case_id, True, "", elapsed, payload.get("specification", {}))


# ---------------------------------------------------------------------------
# Scoring
# ---------------------------------------------------------------------------


def score(outcome: GenerationOutcome, case: EvaluationCase, metrics: Metrics) -> dict[str, Any]:
    metrics.generations += 1
    metrics.total_elapsed_ms += outcome.elapsed_ms
    metrics.behaviours_expected += len(case.expected_behaviours)

    detail: dict[str, Any] = {
        "case": case.case_id,
        "ok": outcome.ok,
        "elapsed_ms": outcome.elapsed_ms,
    }

    if not outcome.ok:
        detail["error"] = outcome.error
        return detail

    problems = validate_specification(outcome.specification, suite=case.suite, max_tests=100)
    outcome.problems = problems
    if not problems:
        metrics.schema_valid += 1
    detail["schema_problems"] = problems

    tests = outcome.specification.get("tests", [])
    metrics.tests_total += len(tests)
    detail["tests"] = len(tests)

    # Per-test validity, so one bad test does not condemn the whole batch.
    valid_tests = 0
    for index, test in enumerate(tests):
        from python.ai.schema import validate_test

        if not validate_test(test, index):
            valid_tests += 1
    metrics.tests_valid += valid_tests
    detail["valid_tests"] = valid_tests

    # Duplicates: same verb, same path, same expectation.
    signatures = [
        (
            str(t.get("method", "")).upper(),
            str(t.get("endpoint", "")),
            int(t.get("expected_status", 0)) if isinstance(t.get("expected_status"), int) else 0,
        )
        for t in tests
        if isinstance(t, dict)
    ]
    duplicates = len(signatures) - len(set(signatures))
    metrics.tests_duplicate += duplicates
    detail["duplicates"] = duplicates

    negatives = sum(
        1
        for t in tests
        if isinstance(t, dict)
        and isinstance(t.get("expected_status"), int)
        and t["expected_status"] >= 400
    )
    metrics.tests_negative += negatives
    detail["negative_tests"] = negatives

    # Coverage by keyword. Crude, and labelled as such: it answers "did anything
    # mention this rule?", not "is the test right?".
    haystack = json.dumps(tests).lower()
    covered = []
    missed = []
    for behaviour in case.expected_behaviours:
        if any(keyword.lower() in haystack for keyword in behaviour.keywords):
            covered.append(behaviour.name)
        else:
            missed.append(behaviour.name)
    metrics.behaviours_covered += len(covered)
    detail["covered"] = covered
    detail["missed"] = missed

    return detail


def execute(
    outcome: GenerationOutcome, testforge: str, base_url: str, metrics: Metrics
) -> dict[str, Any]:
    """Registers the generated suite and runs it against a live service."""
    if not outcome.ok or outcome.problems:
        return {"skipped": "specification was not valid"}

    with tempfile.TemporaryDirectory() as workspace:
        spec_path = Path(workspace) / "spec.json"
        spec_path.write_text(json.dumps(outcome.specification), encoding="utf-8")

        suite = outcome.specification.get("suite", "generated")
        # Registration lives in memory, so loading and running must happen in
        # one invocation: `--execute` does both.
        run = subprocess.run(
            [
                testforge,
                "--no-db",
                "spec",
                "load",
                str(spec_path),
                "--execute",
                "--json",
            ],
            capture_output=True,
            text=True,
            timeout=300,
            check=False,
        )
        try:
            payload = json.loads(run.stdout or "{}")
        except json.JSONDecodeError:
            return {"error": "run output was not JSON"}

        statistics = payload.get("statistics", {})
        total = int(statistics.get("total", 0))
        passed = int(statistics.get("passed", 0))
        metrics.executed += total
        metrics.executed_passed += passed
        return {"suite": suite, "executed": total, "passed": passed}


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument(
        "--endpoint",
        default="http://127.0.0.1:8810",
        help="AI sidecar base URL (ignored with --mock)",
    )
    parser.add_argument(
        "--mock",
        action="store_true",
        help="use TestForge's deterministic mock provider instead of a model",
    )
    parser.add_argument("--testforge", default="./build/testforge", help="path to the CLI")
    parser.add_argument("--max-tests", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--run", action="store_true", help="also execute the generated tests")
    parser.add_argument("--base-url", default="http://127.0.0.1:8000")
    parser.add_argument("--output", default="", help="write the JSON report here")
    parser.add_argument("--case", default="", help="run only this case id")
    args = parser.parse_args()

    cases = EVALUATION_CASES
    if args.case:
        cases = [c for c in cases if c.case_id == args.case]
        if not cases:
            print(f"no such case: {args.case}", file=sys.stderr)
            return 2

    metrics = Metrics()
    details: list[dict[str, Any]] = []

    for case in cases:
        print(f"[eval] {case.case_id} ... ", end="", flush=True)
        if args.mock:
            outcome = generate_via_testforge_mock(case, args.testforge, args.max_tests)
        else:
            outcome = generate_via_sidecar(case, args.endpoint, args.max_tests, args.timeout)

        detail = score(outcome, case, metrics)
        if args.run:
            detail["execution"] = execute(outcome, args.testforge, args.base_url, metrics)
        details.append(detail)

        if outcome.ok:
            print(f"{detail.get('tests', 0)} test(s), {len(detail.get('missed', []))} missed")
        else:
            print(f"FAILED: {outcome.error[:120]}")

    report = {
        "provider": "mock" if args.mock else args.endpoint,
        "metrics": metrics.as_report(),
        "cases": details,
        "notes": [
            "requirement_coverage is a keyword heuristic: it detects a rule nobody "
            "wrote a test for, not whether the test is correct.",
            "execution_pass_rate is only present with --run and a live service.",
        ],
    }

    print()
    print(json.dumps(report["metrics"], indent=2))
    if args.output:
        Path(args.output).write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"\nreport written to {args.output}")

    # A non-zero exit when the model could not produce a usable spec at all,
    # so this can be wired into CI as a regression gate on prompt changes.
    schema_validity = report["metrics"]["schema_validity"]
    if schema_validity is not None and schema_validity < 0.5:
        print("\nschema validity below 50% — treating as a failure", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
