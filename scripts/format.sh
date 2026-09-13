#!/usr/bin/env bash
#
# Format the C++ and Python sources in place.
#
#   scripts/format.sh          # rewrite files
#   scripts/format.sh --check  # report without changing anything (what CI does)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

CHECK=0
[[ "${1:-}" == "--check" ]] && CHECK=1

# --- C++ --------------------------------------------------------------------

CLANG_FORMAT=""
for candidate in clang-format clang-format-18 clang-format-17 clang-format-16 clang-format-15 clang-format-14; do
  if command -v "$candidate" > /dev/null 2>&1; then
    CLANG_FORMAT="$candidate"
    break
  fi
done

if [[ -z "$CLANG_FORMAT" ]]; then
  echo "clang-format not found; skipping the C++ sources" >&2
else
  # The vendored SQLite amalgamation lives in .deps-cache and is not ours.
  mapfile -t SOURCES < <(find include src tests examples benchmarks \
    \( -name '*.cpp' -o -name '*.hpp' \) | sort)

  echo "==> $CLANG_FORMAT on ${#SOURCES[@]} file(s)"
  if [[ $CHECK -eq 1 ]]; then
    "$CLANG_FORMAT" --dry-run --Werror "${SOURCES[@]}"
  else
    "$CLANG_FORMAT" -i "${SOURCES[@]}"
  fi
fi

# --- Python -----------------------------------------------------------------

if command -v ruff > /dev/null 2>&1; then
  echo "==> ruff"
  if [[ $CHECK -eq 1 ]]; then
    ruff format --check python/ sample-service/ scripts/
    ruff check python/ sample-service/ scripts/
  else
    ruff format python/ sample-service/ scripts/
    ruff check --fix python/ sample-service/ scripts/
  fi
else
  echo "ruff not found; skipping the Python sources (pip install ruff)" >&2
fi

echo "done"
