#!/usr/bin/env bash
#
# Run the scheduling benchmark and save the numbers.
#
#   scripts/benchmark.sh                     # default sweep
#   scripts/benchmark.sh --tests 96
#   scripts/benchmark.sh --workload cpu
#
# Results go to benchmarks/results/<host>-<timestamp>.json so a measurement can
# always be traced back to the machine that produced it. That matters: a
# speed-up figure without the hardware it was measured on is not a number, it
# is a slogan.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
BENCH="$BUILD_DIR/testforge_bench"
OUTPUT_DIR="benchmarks/results"

if [[ ! -x "$BENCH" ]]; then
  echo "no benchmark binary at $BENCH; building" >&2
  scripts/build.sh --release --no-tests
fi

mkdir -p "$OUTPUT_DIR"
HOST="$(uname -n | tr -cd '[:alnum:]._-')"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUTPUT="$OUTPUT_DIR/${HOST}-${STAMP}.json"

echo "==> benchmarking (results -> $OUTPUT)"
echo

# A benchmark run on a machine that is busy doing something else measures the
# something else. Say so rather than silently producing a bad number.
if command -v uptime > /dev/null 2>&1; then
  echo "load before the run: $(uptime | sed 's/.*load average: //')"
  echo
fi

"$BENCH" --json "$OUTPUT" "$@"

echo
echo "saved to $OUTPUT"
echo
echo "These numbers describe this machine at this moment. Comparing them with"
echo "numbers from a shared CI runner, or from a laptop on battery, is not"
echo "meaningful."
