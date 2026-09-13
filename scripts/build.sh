#!/usr/bin/env bash
#
# Configure and build TestForge.
#
#   scripts/build.sh                    # RelWithDebInfo, tests on
#   scripts/build.sh --debug            # Debug
#   scripts/build.sh --release          # Release
#   scripts/build.sh --asan             # AddressSanitizer + UBSan
#   scripts/build.sh --tsan             # ThreadSanitizer
#   scripts/build.sh --no-tests         # skip the test-suite (faster)
#   scripts/build.sh --clean            # start from an empty build directory
#
# The first build downloads GoogleTest and the SQLite amalgamation into
# .deps-cache/ and needs network access; later builds do not.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="RelWithDebInfo"
BUILD_TESTS="ON"
EXTRA_FLAGS=()
CLEAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug)    BUILD_TYPE="Debug" ;;
    --release)  BUILD_TYPE="Release" ;;
    --asan)     EXTRA_FLAGS+=(-DTESTFORGE_SANITIZE_ADDRESS=ON -DTESTFORGE_SANITIZE_UB=ON)
                BUILD_TYPE="Debug"
                BUILD_DIR="${BUILD_DIR}-asan" ;;
    --tsan)     EXTRA_FLAGS+=(-DTESTFORGE_SANITIZE_THREAD=ON)
                BUILD_TYPE="Debug"
                BUILD_DIR="${BUILD_DIR}-tsan" ;;
    --werror)   EXTRA_FLAGS+=(-DTESTFORGE_WERROR=ON) ;;
    --tidy)     EXTRA_FLAGS+=(-DTESTFORGE_ENABLE_CLANG_TIDY=ON) ;;
    --no-tests) BUILD_TESTS="OFF" ;;
    --clean)    CLEAN=1 ;;
    -h|--help)  sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)          echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

if [[ $CLEAN -eq 1 ]]; then
  echo "removing $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

# Ninja when it is available: it is faster and its output is easier to read.
GENERATOR=()
if command -v ninja > /dev/null 2>&1; then
  GENERATOR=(-G Ninja)
fi

echo "==> configuring ($BUILD_TYPE, tests=$BUILD_TESTS) in $BUILD_DIR"
cmake -S . -B "$BUILD_DIR" "${GENERATOR[@]}" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DTESTFORGE_BUILD_TESTS="$BUILD_TESTS" \
  -DTESTFORGE_BUILD_EXAMPLES=ON \
  -DTESTFORGE_BUILD_BENCHMARKS=ON \
  "${EXTRA_FLAGS[@]}"

echo "==> building"
cmake --build "$BUILD_DIR" --parallel

echo
echo "built:"
for binary in testforge testforge_tests testforge_bench; do
  if [[ -x "$BUILD_DIR/$binary" ]]; then
    printf '  %-18s %s\n' "$binary" "$BUILD_DIR/$binary"
  fi
done

echo
echo "next:"
echo "  $BUILD_DIR/testforge list"
echo "  $BUILD_DIR/testforge run --suite smoke"
if [[ "$BUILD_TESTS" == "ON" ]]; then
  echo "  ctest --test-dir $BUILD_DIR --output-on-failure"
fi

# ThreadSanitizer needs two pieces of local knowledge that are easy to lose an
# afternoon to, so say them here rather than in a document nobody reads first.
if [[ " ${EXTRA_FLAGS[*]} " == *"SANITIZE_THREAD=ON"* ]]; then
  cat <<TSAN

ThreadSanitizer notes:

  export TSAN_OPTIONS="suppressions=$REPO_ROOT/.tsan-suppressions"

    Silences a libstdc++ mutex-lifecycle false positive. Read the file — it
    explains itself, and it suppresses no data race.

  setarch \$(uname -m) -R $BUILD_DIR/testforge_tests

    Only needed if TSan aborts with "unexpected memory mapping". Recent
    kernels use more ASLR entropy than TSan's shadow mapping expects; -R
    disables randomisation for the child.
TSAN
fi
