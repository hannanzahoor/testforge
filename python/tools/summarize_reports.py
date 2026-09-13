"""Summarise TestForge JSON reports.

Two jobs that are awkward in C++ and natural in Python:

  * roll several report files into one view, which is what you want after a
    sharded CI run where each shard wrote its own report;
  * emit a GitHub Actions job summary, so a failing run explains itself on the
    workflow page instead of only inside a log.

Usage::

    python -m python.tools.summarize_reports reports/*.json
    python -m python.tools.summarize_reports reports/latest.json --markdown
    python -m python.tools.summarize_reports reports/*.json --github-summary
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys
from collections import Counter
from collections.abc import Iterable
from pathlib import Path
from typing import Any

STATUS_ICON = {
    "PASSED": "PASS",
    "FAILED": "FAIL",
    "SKIPPED": "SKIP",
    "ERROR": "ERR",
    "TIMEOUT": "TIME",
}


def load_reports(patterns: Iterable[str]) -> list[dict[str, Any]]:
    reports: list[dict[str, Any]] = []
    for pattern in patterns:
        matches = sorted(glob.glob(pattern)) or ([pattern] if os.path.exists(pattern) else [])
        if not matches:
            print(f"warning: no files match {pattern!r}", file=sys.stderr)
        for path in matches:
            try:
                document = json.loads(Path(path).read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:
                print(f"warning: skipping {path}: {exc}", file=sys.stderr)
                continue
            if "results" not in document:
                print(f"warning: {path} is not a TestForge run report", file=sys.stderr)
                continue
            document["_path"] = path
            reports.append(document)
    return reports


def merge(reports: list[dict[str, Any]]) -> tuple[dict[str, int], list[dict[str, Any]]]:
    totals = Counter()
    failures: list[dict[str, Any]] = []

    for report in reports:
        for result in report.get("results", []):
            status = result.get("status", "ERROR")
            totals[status] += 1
            totals["TOTAL"] += 1
            if status in ("FAILED", "ERROR", "TIMEOUT"):
                failures.append(result)
            if result.get("flaky_candidate"):
                totals["FLAKY"] += 1

    return dict(totals), failures


def success_rate(totals: dict[str, int]) -> float:
    considered = totals.get("TOTAL", 0) - totals.get("SKIPPED", 0)
    if considered <= 0:
        return 0.0
    return 100.0 * totals.get("PASSED", 0) / considered


def render_text(
    reports: list[dict[str, Any]], totals: dict[str, int], failures: list[dict[str, Any]]
) -> str:
    lines: list[str] = []
    lines.append(f"TestForge summary over {len(reports)} report(s)")
    lines.append("=" * 60)
    for key in ("TOTAL", "PASSED", "FAILED", "SKIPPED", "ERROR", "TIMEOUT"):
        lines.append(f"  {key:<9} {totals.get(key, 0)}")
    lines.append(f"  {'RATE':<9} {success_rate(totals):.1f}%")

    if totals.get("FLAKY"):
        lines.append(f"  {'FLAKY':<9} {totals['FLAKY']} (passed only after a retry)")

    categories = Counter(f.get("failure_category", "UNKNOWN") for f in failures)
    if categories:
        lines.append("")
        lines.append("Failure categories")
        for category, count in categories.most_common():
            lines.append(f"  {count:>4}  {category}")

    if failures:
        lines.append("")
        lines.append("Failures")
        for failure in failures[:40]:
            lines.append(
                f"  {STATUS_ICON.get(failure.get('status', ''), '?'):<5} "
                f"{failure.get('qualified_name', '?')}  "
                f"{failure.get('error_message', '')[:100]}"
            )
        if len(failures) > 40:
            lines.append(f"  ... and {len(failures) - 40} more")

    return "\n".join(lines)


def render_markdown(
    reports: list[dict[str, Any]], totals: dict[str, int], failures: list[dict[str, Any]]
) -> str:
    rate = success_rate(totals)
    verdict = "PASS" if not failures else "FAIL"

    lines: list[str] = []
    lines.append(f"## TestForge: {verdict}")
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("| --- | ---: |")
    lines.append(f"| Total | {totals.get('TOTAL', 0)} |")
    lines.append(f"| Passed | {totals.get('PASSED', 0)} |")
    lines.append(f"| Failed | {totals.get('FAILED', 0)} |")
    lines.append(f"| Errors | {totals.get('ERROR', 0)} |")
    lines.append(f"| Timeouts | {totals.get('TIMEOUT', 0)} |")
    lines.append(f"| Skipped | {totals.get('SKIPPED', 0)} |")
    lines.append(f"| Success rate | {rate:.1f}% |")
    lines.append("")

    categories = Counter(f.get("failure_category", "UNKNOWN") for f in failures)
    if categories:
        lines.append("### Failure categories")
        lines.append("")
        lines.append("| Category | Count |")
        lines.append("| --- | ---: |")
        for category, count in categories.most_common():
            lines.append(f"| `{category}` | {count} |")
        lines.append("")

    if failures:
        lines.append("### Failing tests")
        lines.append("")
        for failure in failures[:25]:
            name = failure.get("qualified_name", "?")
            category = failure.get("failure_category", "UNKNOWN")
            message = (failure.get("error_message") or "").replace("|", r"\|")[:160]
            lines.append(f"<details><summary><code>{name}</code> — {category}</summary>")
            lines.append("")
            lines.append(f"{message}")
            detail = (failure.get("error_detail") or "")[:1500]
            if detail:
                lines.append("")
                lines.append("```")
                lines.append(detail)
                lines.append("```")
            lines.append("")
            lines.append("</details>")
            lines.append("")
        if len(failures) > 25:
            lines.append(f"_...and {len(failures) - 25} more._")

    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("reports", nargs="+", help="report files or glob patterns")
    parser.add_argument("--markdown", action="store_true", help="emit Markdown")
    parser.add_argument(
        "--github-summary",
        action="store_true",
        help="append Markdown to $GITHUB_STEP_SUMMARY (implies --markdown)",
    )
    parser.add_argument(
        "--fail-on-failures",
        action="store_true",
        help="exit non-zero when any test failed",
    )
    args = parser.parse_args()

    reports = load_reports(args.reports)
    if not reports:
        print("no readable reports", file=sys.stderr)
        return 2

    totals, failures = merge(reports)
    text = (
        render_markdown(reports, totals, failures)
        if (args.markdown or args.github_summary)
        else render_text(reports, totals, failures)
    )
    print(text)

    if args.github_summary:
        summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
        if summary_path:
            with open(summary_path, "a", encoding="utf-8") as handle:
                handle.write(text + "\n")
        else:
            print(
                "note: GITHUB_STEP_SUMMARY is not set; printed to stdout instead",
                file=sys.stderr,
            )

    if args.fail_on_failures and failures:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
