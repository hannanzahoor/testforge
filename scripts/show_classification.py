#!/usr/bin/env python3
"""Print a per-test status/category table from a TestForge JSON report.

Used by scripts/demo.sh to show what the failure-injection suite produced. A
separate file rather than a shell one-liner because quoting a table-formatting
script inside bash is how you get a demo that breaks on somebody else's shell.

    python3 scripts/show_classification.py reports/latest.json
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {Path(argv[0]).name} <report.json>", file=sys.stderr)
        return 2

    try:
        report = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"cannot read {argv[1]}: {exc}", file=sys.stderr)
        return 1

    results = report.get("results", [])
    if not results:
        print("  (no results in the report)")
        return 0

    name_width = max(len(r.get("test_name", "")) for r in results)
    status_width = max(len(r.get("status", "")) for r in results)

    for result in sorted(results, key=lambda r: r.get("test_name", "")):
        name = result.get("test_name", "?").ljust(name_width)
        status = result.get("status", "?").ljust(status_width)
        category = result.get("failure_category", "?")
        marker = " " if status.strip() in ("PASSED", "SKIPPED") else "!"
        print(f"  {marker} {name}  {status}  {category}")

    categories: dict[str, int] = {}
    for result in results:
        category = result.get("failure_category", "UNKNOWN")
        if category != "NONE":
            categories[category] = categories.get(category, 0) + 1

    if categories:
        print()
        print("  distinct failure categories exercised: " + str(len(categories)))
        for category, count in sorted(categories.items(), key=lambda kv: -kv[1]):
            print(f"    {count:>3}  {category}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
