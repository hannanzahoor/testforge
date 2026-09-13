# Contributing

## Getting set up

TestForge builds on Linux with GCC 11+ or Clang 14+ and CMake 3.20+. GoogleTest
and the SQLite amalgamation are fetched by CMake on the first configure, so
there are no system packages to install first.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTESTFORGE_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`scripts/build.sh` wraps the common configurations:

```bash
scripts/build.sh              # RelWithDebInfo, tests on
scripts/build.sh --werror     # warnings as errors, as CI builds it
scripts/build.sh --asan       # AddressSanitizer + UndefinedBehaviorSanitizer
scripts/build.sh --tsan       # ThreadSanitizer
scripts/build.sh --clean      # start from an empty build directory
```

## Before opening a pull request

Run the same checks CI runs.

```bash
# Build without warnings
scripts/build.sh --werror
ctest --test-dir build-werror --output-on-failure

# Formatting is enforced, not advisory
clang-format-14 --dry-run --Werror \
  $(find include src tests examples benchmarks -name '*.cpp' -o -name '*.hpp')

# Python
pytest sample-service/tests -q
ruff check python/ sample-service/
ruff format --check python/ sample-service/
```

At least one sanitizer configuration should pass for any change touching
threading, lifetimes, or the network layer:

```bash
scripts/build.sh --asan && ctest --test-dir build-asan --output-on-failure
```

ThreadSanitizer needs the suppressions file, which covers a libstdc++
mutex-lifecycle false positive and nothing else:

```bash
scripts/build.sh --tsan
export TSAN_OPTIONS="suppressions=$PWD/.tsan-suppressions"
ctest --test-dir build-tsan --output-on-failure
```

On kernels with high ASLR entropy, ThreadSanitizer needs
`setarch $(uname -m) -R` in front of the test binary. `scripts/build.sh --tsan`
prints the exact command.

## Code style

Formatting is handled by `.clang-format` (Google base, 100 columns, 4-space
indent) and enforced by CI. Run `scripts/format.sh` to apply it.

A few conventions the formatter cannot enforce:

- Comments explain *why*, not *what*. A comment that restates the line above it
  is noise; one that records an ownership assumption, a concurrency invariant,
  or a rejected alternative is worth keeping.
- Every foreign resource gets an RAII wrapper. There are no bare `close()`,
  `sqlite3_finalize()`, or `freeaddrinfo()` calls on a path that can throw or
  return early.
- Prefer `unique_ptr`. Reach for `shared_ptr` only where ownership is genuinely
  shared, and say why in a comment.
- New public API needs a test. New behaviour that fixes a bug needs a test that
  fails without the fix.

## Tests

There are two separate bodies of tests and they are not interchangeable.

| | `tests/` | `examples/sample_tests/` |
|---|---|---|
| Subject | TestForge itself | a service, exercised through TestForge |
| Framework | GoogleTest | TestForge |
| Run by | `ctest` | `testforge run` |

TestForge is deliberately not tested with itself: a bug in the assertion engine
would make a self-hosted suite report success.

Inside a TestForge test body use `TF_ASSERT_*`, not GoogleTest's `ASSERT_*`.
The short forms are opt-in via `testforge/testing/ShortAssertions.hpp` because
they collide with GoogleTest's macros.

The `failure_injection` suite is expected to fail. CI asserts that it exits `1`
and produces every failure category it advertises, so do not "fix" it.

## Reporting a security issue

See [SECURITY.md](SECURITY.md).
