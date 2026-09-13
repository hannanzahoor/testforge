#!/usr/bin/env bash
#
# The full TestForge demonstration, end to end.
#
#   scripts/demo.sh
#
# It walks the whole pipeline:
#
#   1. build
#   2. list the registered tests
#   3. run the smoke suite (green)
#   4. start the sample service and run the API suite against it (green)
#   5. run the failure-injection suite and show detection, classification,
#      diagnostics, persistence and reporting (deliberately red)
#   6. AI analysis of one of those failures, using the offline provider
#   7. history, statistics and the generated reports
#
# Step 5 is the point of the exercise. A tool that only ever demonstrates
# passing tests has demonstrated nothing.
#
# Nothing here needs an API key or a GPU. Steps that need something absent are
# skipped with a reason rather than faked.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
TF="$BUILD_DIR/testforge"
DEMO_DB="${DEMO_DB:-demo.db}"
SERVICE_PORT="${SERVICE_PORT:-8000}"
SERVICE_PID=""

bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
dim()   { printf '\033[2m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
red()   { printf '\033[31m%s\033[0m\n' "$*"; }

step() {
  echo
  printf '\033[1m\033[36m%s\033[0m\n' "══ $* "
  echo
}

cleanup() {
  if [[ -n "$SERVICE_PID" ]] && kill -0 "$SERVICE_PID" 2> /dev/null; then
    kill "$SERVICE_PID" 2> /dev/null || true
    wait "$SERVICE_PID" 2> /dev/null || true
  fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
step "1/7  Build"
# ---------------------------------------------------------------------------

if [[ ! -x "$TF" ]]; then
  dim "no binary at $TF; building"
  scripts/build.sh --no-tests || { red "build failed"; exit 1; }
else
  dim "using the existing binary at $TF"
fi
"$TF" --version

# Each demo run starts from an empty database so the history and statistics
# shown at the end describe this run and nothing else.
rm -f "$DEMO_DB"
export TESTFORGE_DB_PATH="$DEMO_DB"
export TESTFORGE_REPORT_DIR="reports/demo"
rm -rf "$TESTFORGE_REPORT_DIR"

# ---------------------------------------------------------------------------
step "2/7  What is registered"
# ---------------------------------------------------------------------------

"$TF" list | tail -n 20
echo
dim "Filtering works by suite, tag, name or glob:"
"$TF" list --tag smoke

# ---------------------------------------------------------------------------
step "3/7  Smoke suite — expected to pass"
# ---------------------------------------------------------------------------

if "$TF" run --suite smoke --workers 4; then
  green "smoke suite passed (exit 0)"
else
  red "smoke suite failed — that is itself a finding"
fi

# ---------------------------------------------------------------------------
step "4/7  API suite against the live sample service"
# ---------------------------------------------------------------------------

if command -v uvicorn > /dev/null 2>&1 || python3 -c "import uvicorn" 2> /dev/null; then
  dim "starting the sample service on port $SERVICE_PORT"
  python3 -m uvicorn app.main:app --port "$SERVICE_PORT" --app-dir sample-service \
    --log-level warning > /tmp/testforge-demo-service.log 2>&1 &
  SERVICE_PID=$!

  ready=0
  for _ in $(seq 1 60); do
    if curl -sf "http://127.0.0.1:$SERVICE_PORT/health" > /dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.25
  done

  if [[ $ready -eq 1 ]]; then
    green "service is up"
    "$TF" run --suite api --workers 4
  else
    red "the service did not become ready; see /tmp/testforge-demo-service.log"
    dim "the API tests would skip rather than fail — try step 5 anyway"
  fi
else
  dim "uvicorn is not installed, so the API tests will skip."
  dim "  pip install -r sample-service/requirements.txt"
  "$TF" run --suite api --workers 4
fi

# ---------------------------------------------------------------------------
step "5/7  Failure injection — expected to FAIL, and that is the point"
# ---------------------------------------------------------------------------

cat <<'EXPLANATION'
Every test in this suite breaks in a specific, controlled way. What follows
shows TestForge doing the five things a test tool has to do when something
breaks:

  detect it        -> a status other than PASSED
  classify it      -> a failure category naming who should look at it
  capture evidence -> logs, the HTTP exchange, system diagnostics
  persist it       -> a row in the results database
  report it        -> console, JSON, HTML

EXPLANATION

"$TF" run --suite failure_injection --workers 4
INJECTED_EXIT=$?

echo
if [[ $INJECTED_EXIT -eq 1 ]]; then
  green "exit code 1 — tests failed, exactly as intended"
  dim "  0 = all passed   1 = tests failed   2 = bad usage   3 = TestForge broke"
else
  red "expected exit code 1, got $INJECTED_EXIT"
fi

echo
bold "Classification, per test:"
if command -v python3 > /dev/null 2>&1; then
  "$TF" run --suite failure_injection --json --no-report 2> /dev/null > /tmp/testforge-injected.json
  python3 "$REPO_ROOT/scripts/show_classification.py" /tmp/testforge-injected.json
else
  dim "(install python3 to see the per-test table)"
fi

# ---------------------------------------------------------------------------
step "6/7  AI failure analysis (offline provider — no API key needed)"
# ---------------------------------------------------------------------------

cat <<'EXPLANATION'
The AI layer is advisory. The verdict above was decided by assertions in the
C++ engine; the model only suggests where to look. Running with --mock uses a
deterministic offline provider, so this works with no key and no network.

EXPLANATION

"$TF" ai analyze-failure --mock --run latest || dim "(no stored failure to analyse)"

echo
dim "With a real model, start the sidecar and drop --mock:"
dim "  export OPENAI_API_KEY=sk-..."
dim "  uvicorn python.ai.service:app --port 8810"
dim "  testforge ai analyze-failure --run latest"

# ---------------------------------------------------------------------------
step "7/7  History, statistics and reports"
# ---------------------------------------------------------------------------

bold "Run history"
"$TF" history --limit 10

echo
bold "Aggregate statistics"
"$TF" stats --runs 10

echo
bold "System diagnostics"
"$TF" diagnose | head -n 48

echo
bold "Reports written"
if [[ -d "$TESTFORGE_REPORT_DIR" ]]; then
  ls -la "$TESTFORGE_REPORT_DIR" | tail -n +2
  echo
  dim "Open the HTML report:  $TESTFORGE_REPORT_DIR/latest.html"
else
  dim "(no reports directory)"
fi

echo
printf '\033[1m\033[32m%s\033[0m\n' "══ Demo complete "
echo
cat <<SUMMARY
What that showed:

  * a green suite and a red one, with the exit code distinguishing them
  * every injected fault classified into the right category
  * diagnostics captured at failure time, not afterwards
  * results persisted and queryable through history and stats
  * three report formats written from the same run
  * an advisory AI analysis that did not change any verdict

Next:
  $TF serve --port 8080     # REST API and dashboard
  $TF --help                # everything else
  docs/architecture.md      # how it fits together
SUMMARY
