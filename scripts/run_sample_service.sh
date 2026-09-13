#!/usr/bin/env bash
#
# Start the sample service (the system under test).
#
#   scripts/run_sample_service.sh            # foreground, port 8000
#   scripts/run_sample_service.sh --port 9000
#   scripts/run_sample_service.sh --reload   # restart on source changes
#
# The TestForge `api` suite points at http://127.0.0.1:8000 by default. Change
# it with --port here and TESTFORGE_API_BASE_URL there.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PORT=8000
RELOAD=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)    PORT="$2"; shift ;;
    --reload)  RELOAD=(--reload) ;;
    -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)         echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

if ! python3 -c "import fastapi, uvicorn" 2> /dev/null; then
  echo "The sample service needs FastAPI and uvicorn:" >&2
  echo "    pip install -r sample-service/requirements.txt" >&2
  exit 1
fi

echo "sample service on http://127.0.0.1:$PORT"
echo "  health     http://127.0.0.1:$PORT/health"
echo "  docs       http://127.0.0.1:$PORT/docs"
echo "  always-500 http://127.0.0.1:$PORT/boom"
echo

exec python3 -m uvicorn app.main:app \
  --host 127.0.0.1 \
  --port "$PORT" \
  --app-dir sample-service \
  "${RELOAD[@]}"
