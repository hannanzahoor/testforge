#!/usr/bin/env bash
#
# Start the AI sidecar.
#
#   export OPENAI_API_KEY=sk-...
#   scripts/run_ai_sidecar.sh
#
# The sidecar holds the API key and the prompts; the C++ engine talks to it
# over plain HTTP on loopback and never sees a credential. See
# docs/ai-architecture.md for why the split exists.
#
# It starts without a key and reports ready=false, so `testforge ai status`
# can tell you exactly what is missing instead of hanging.
#
# For a demo with no key at all, skip this entirely and use the offline
# deterministic provider:
#
#   testforge ai generate-tests "..." --mock

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PORT="${TESTFORGE_AI_PORT:-8810}"
RELOAD=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)    PORT="$2"; shift ;;
    --reload)  RELOAD=(--reload) ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)         echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

if ! python3 -c "import fastapi, uvicorn" 2> /dev/null; then
  echo "The sidecar needs FastAPI and uvicorn:" >&2
  echo "    pip install -r python/requirements.txt" >&2
  exit 1
fi

# Load .env for convenience, without letting it override the real environment.
if [[ -f .env ]]; then
  set -a
  # shellcheck disable=SC1091
  . ./.env
  set +a
fi

if [[ -z "${OPENAI_API_KEY:-}" ]]; then
  echo "note: OPENAI_API_KEY is not set."
  echo "      The sidecar will start and report ready=false; live generation and"
  echo "      analysis will return a clear 'not configured' error rather than"
  echo "      inventing a response."
  echo
fi

echo "AI sidecar on http://127.0.0.1:$PORT"
echo "  health  http://127.0.0.1:$PORT/health"
echo "  model   ${TESTFORGE_AI_MODEL:-gpt-4o-mini}"
echo
echo "Then, in another terminal:"
echo "  export TESTFORGE_AI_ENABLED=1"
echo "  testforge ai status"
echo

exec python3 -m uvicorn python.ai.service:app \
  --host 127.0.0.1 \
  --port "$PORT" \
  "${RELOAD[@]}"
