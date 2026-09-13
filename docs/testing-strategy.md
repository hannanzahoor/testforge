# Testing strategy

How a test framework is itself tested, and why the failure-injection suite is
the most important part of this repository.

- [Two layers](#two-layers)
- [The framework's own suite](#the-frameworks-own-suite)
- [Failure injection](#failure-injection)
- [The bundled suites](#the-bundled-suites)
- [The sample service](#the-sample-service)
- [What is deliberately not tested](#what-is-deliberately-not-tested)
- [Running everything](#running-everything)

---

## Two layers

There are two distinct bodies of tests, and confusing them is easy:

| | `tests/` | `examples/sample_tests/` |
|---|---|---|
| Tests | **TestForge itself** | a system under test, using TestForge |
| Framework | GoogleTest | TestForge |
| Run by | `ctest` | `testforge run` |
| Count | 395 cases | 59 tests in 6 suites |
| Purpose | correctness of the engine | demonstration, and a real workload |

They use different assertion libraries on purpose: **the thing under test cannot
also be the thing doing the checking.** If a bug in `AssertionEngine` made every
assertion pass, a self-hosted suite would report success. GoogleTest is the
independent observer.

That separation is why `ASSERT_*` is opt-in
(`testforge/testing/ShortAssertions.hpp`) rather than defined by default — the
short names would collide with GoogleTest's, and the header raises a compile
error if both are included.

---

## The framework's own suite

395 cases across 56 fixtures.

```
tests/
├── unit/
│   ├── JsonTest.cpp            parser, serialiser, limits, round-trips
│   ├── CoreTest.cpp            strings, clock, ids, status, config, env, logging
│   ├── TestingTest.cpp         assertions, soft assertions, registry, selector
│   ├── ExecutionTest.cpp       thread pool, cancellation, failure classifier
│   ├── NetTest.cpp             URL parsing, URL policy, headers, responses
│   ├── DiagnosticsTest.cpp     process execution, /proc reading, GPU parsing
│   ├── PersistenceTest.cpp     SQLite wrappers, migrations, repository, analytics
│   ├── AiTest.cpp              spec parsing, validation, providers, failure context
│   └── ReportingTest.cpp       statistics and all four reporters
├── integration/
│   ├── RunnerTest.cpp          the whole engine, against an in-memory repository
│   └── HttpTest.cpp            the client and the server, pointed at each other
├── failure_injection/
│   └── FailureInjectionTest.cpp  asserts the triage pipeline
└── fixtures/
    └── InMemoryRepository.hpp
```

### What the unit tests aim at

Not line coverage — the cases where a plausible implementation is subtly wrong:

**JSON.** Surrogate pairs recombined into one code point; lone surrogates
replaced rather than emitted as invalid UTF-8; 2⁵³+1 surviving as an integer;
doubles round-tripping exactly; `42.0` not degrading to `42`; non-finite values
becoming `null`; fourteen malformed documents that must be rejected; a depth cap
against stack exhaustion.

**Assertions.** Mixed-sign comparison giving the mathematically correct answer
(`ASSERT_LT(-1, someSize)` passes, as a test author expects); strings quoted so
`"abc "` and `"abc"` are distinguishable in a failure; a 5000-character body
truncated in the message; soft assertions collecting all failures; the soft-scope
destructor never throwing during unwinding.

**Selector.** Different filter fields combining with AND, not OR
(`--suite api --tag smoke` narrows); a seeded shuffle reproducing exactly; shard
membership stable when an unrelated test is added.

**Classifier.** Every typed exception mapping to its category; a 5xx promoting an
assertion failure to an application failure; a 404 *not* promoting; refinement
never making a diagnosis vaguer.

**Persistence.** `Robert'); DROP TABLE t;--` surviving as data; bound strings
copied (`SQLITE_TRANSIENT`); a transaction rolling back when not committed; the
flake heuristic requiring both alternation *and* a minimum run count.

**AI.** Every hostile specification shape rejected — absolute URLs, traversal,
request splitting, credential headers, `CONNECT`, unsafe names, duplicates.

### Integration

`RunnerTest.cpp` drives a real `TestRunner` over a real registry with real
threads, and asserts on properties that only exist at that level:

- one failing test does not stop the other nine;
- a `throw 42;` cannot take the process down;
- results are persisted *as each test finishes*, not batched — `InMemoryRepository`
  counts the calls;
- a persistence outage does not fail the run;
- diagnostics attach to failures and not to passes;
- transient failures are retried and flagged as flaky candidates; deterministic
  ones are not retried at all;
- report order is deterministic even when completion order is not.

`HttpTest.cpp` points TestForge's HTTP client at TestForge's HTTP server on port
0, which exercises request parsing, routing, path parameters, chunked decoding,
timeouts, cancellation, size caps and concurrent clients without an external
service.

---

## Failure injection

**This is the part that matters.** A framework that only demonstrates passing
tests has demonstrated nothing.

There are two halves.

### The demonstration — `examples/sample_tests/FailureInjectionTests.cpp`

Fifteen tests that break on purpose, each named for the category it should
produce. Running them shows a human the whole triage pipeline:

```
$ testforge run --suite failure_injection

  ! assertion_equality                FAILED    ASSERTION_FAILURE
  ! assertion_on_text                 FAILED    ASSERTION_FAILURE
  ! multiple_soft_assertions          FAILED    ASSERTION_FAILURE
  ! connection_refused                ERROR     NETWORK_FAILURE
  ! missing_dependency                ERROR     DEPENDENCY_FAILURE
  ! environment_failure               ERROR     ENVIRONMENT_FAILURE
  ! failure_in_setup                  ERROR     RESOURCE_FAILURE
  ! invalid_configuration             ERROR     CONFIGURATION_FAILURE
  ! unexpected_std_exception          ERROR     UNKNOWN
  ! non_std_exception                 ERROR     UNKNOWN
  ! cooperative_timeout               TIMEOUT   TIMEOUT
  ! uncooperative_timeout             TIMEOUT   TIMEOUT
    control_test_passes               PASSED    NONE
```

(Two more — `server_error_is_application_failure` and
`not_found_is_a_plain_assertion_failure` — skip unless the sample service is
running, then produce `APPLICATION_FAILURE` and `ASSERTION_FAILURE`.)

Excluded from a bare `testforge run` via
`execution.exclude_suites_by_default`, so a healthy checkout stays green.

### The assertion — `tests/failure_injection/FailureInjectionTest.cpp`

The demonstration shows; this **proves**. Eighteen GoogleTest cases that inject
each fault through a real `TestRunner` and assert on the result:

```cpp
TEST_F(InjectionFixture, ApplicationFailureIsPromotedFromAnAssertion) {
    inject("application", [](TestContext& ctx) {
        ctx.addMetadata("http_status", 503);
        TF_ASSERT_EQ(503, 200);
    });

    const TestResult result = runOne("application");
    EXPECT_EQ(result.status, TestStatus::Failed);
    EXPECT_EQ(result.failureCategory, FailureCategory::ApplicationFailure);
}
```

It also asserts the pipeline *around* the failure: diagnostics attached to
failures and not to passes, logs captured, results persisted, all four report
formats rendering the failure, and the AI failure context carrying the evidence
while the verdict stays the engine's.

The last case runs every injected fault at once with four workers and checks the
run completes with the right counts and exit code — a crash, a hang or a lost
result would fail it.

### And in CI

`.github/workflows/tests.yml` asserts the **negative**: the injected suite must
exit `1`, and must produce the categories it advertises.

```bash
./build/testforge run --suite failure_injection --json --no-report > /tmp/injected.json
# exit must be 1, and the categories must include ASSERTION_FAILURE, TIMEOUT,
# NETWORK_FAILURE, DEPENDENCY_FAILURE, ENVIRONMENT_FAILURE, RESOURCE_FAILURE
# and CONFIGURATION_FAILURE.
```

Without this, a regression that made TestForge stop detecting failures would
turn CI *greener*, and nobody would notice.

---

## The bundled suites

59 tests in six suites, which are both a demonstration and a real workload for
the engine.

| Suite | Tests | What it exercises |
|---|---:|---|
| `smoke` | 6 | The framework itself: assertions, config, JSON, cancellation, redaction. No dependencies. |
| `api` | 17 | The sample service over real HTTP: happy paths, negative cases, boundaries, malformed input, auth, latency, a full CRUD lifecycle. |
| `system` | 10 | Linux diagnostics against the real machine, plus the no-shell guarantee and subprocess timeouts. |
| `gpu` | 6 | NVIDIA diagnostics. Skips without hardware. |
| `perf` | 5 | Guard rails: JSON throughput, thread-pool correctness under contention, assertion overhead. |
| `failure_injection` | 15 | Deliberate failures, as above. |

The API suite is written the way an API suite should be: for each rule the
service documents, a test that satisfies it and a test that violates it, plus
both sides of every numeric boundary. A password of exactly 8 characters must be
accepted; 7 must be rejected.

Tests that need something absent **skip with a reason** rather than failing.
A missing sample service is not a defect in the sample service.

---

## The sample service

`sample-service/` is a FastAPI application with 27 pytest cases of its own.

Two suites test the same service on purpose. The pytest suite checks it against
its contract using the framework its authors would reach for; TestForge's `api`
suite checks it from outside, over real HTTP, with its own client. When the two
disagree, the disagreement is informative — it usually means the contract and
the wire behaviour have diverged.

The service is deliberately *interesting*: documented validation rules so
boundary tests have a boundary, `409` on conflict, `/boom` that always returns
500, `/slow?delay_ms=` for latency and timeout tests, `/flaky?failure_rate=` for
flake detection, and a token-protected endpoint.

---

## Regression tests

A regression test is written against a bug that actually happened, and it is
named after the behaviour rather than the fix, so the reason survives.

| Test | The bug it pins |
|---|---|
| `Strings.RedactionIsIdempotent` | `redactSecrets` recursed on its own mask and blew the stack |
| `RunnerFixture.FinishingInsideTheGracePeriodIsStillATimeout` | a late-finishing test was reported `PASSED` |
| `HttpFixture.ResponseSizeIsCapped` | the body cap was applied while reading headers |
| `RunnerFixture.AnOrphanedTestMayOutliveTheConfigThatStartedIt` | `TestContext` held a dangling `Config*` |
| `RunnerFixture.RunCancellationReachesATestWithNoDeadline` | an unbounded test could not observe Ctrl-C |
| `RunnerFixture.ACancelledRunDoesNotExitZero` | an interrupted run reported success |
| `RedirectFixture.CrossOriginRedirectDropsAuthorization` | credentials leaked across a redirect |
| `RedirectFixture.SameOriginRedirectKeepsCredentials` | found that root-relative redirects resolved to the wrong path |
| `ThreadPool.ConcurrentShutdownCallsAreSafe` | two `shutdown()` callers joined the same thread |
| `Interrupt.ObservesSigterm` | handlers were installed but the flag was never read |

**A regression test is only proven once it has been watched failing.** For the
use-after-free the test was written first, then the fix was reverted and the
same ASan binary rebuilt with one line changed, to watch it fail with a
`heap-use-after-free` before watching it pass. That exercise is what turns
"I added a test" into "the test catches the bug".

---

## Sanitizers

The whole suite runs under two configurations, and both are clean:

```bash
scripts/build.sh --asan && ctest --test-dir build-asan --output-on-failure
scripts/build.sh --tsan
export TSAN_OPTIONS="suppressions=$PWD/.tsan-suppressions"
ctest --test-dir build-tsan --output-on-failure
```

| Configuration | Options | Result |
|---|---|---|
| ASan + UBSan | `detect_leaks=1:halt_on_error=1`, `print_stacktrace=1` | 0 reports, 395/395 |
| ThreadSanitizer | `halt_on_error=1:second_deadlock_stack=1` + suppressions | 0 warnings, 395/395 |

Leak detection is deliberately **on**. It is what caught 1743 bytes leaked by an
earlier `std::async`-based timeout design, and the response was to change the
design rather than suppress the report.

Two suppressions are in force, scoped to two function names, for a libstdc++
mutex-lifecycle false positive. **No data race is suppressed** — and narrowing
that suppression instead of silencing the file is exactly what exposed two real
races in `HttpServer::stop()`. See [concurrency.md](concurrency.md).

**On recent kernels TSan needs** `setarch $(uname -m) -R`, because their ASLR
entropy exceeds what TSan's shadow mapping expects. `scripts/build.sh --tsan`
prints this.

---

## Concurrency tests

Threading bugs do not show up in a test that runs one thing at a time. These are
written so that a regression *fails* rather than passes quietly:

| Property | Test | Why it would catch a regression |
|---|---|---|
| Every task runs exactly once | `ThreadPool.RunsEveryTaskExactlyOnce` | checks the **sum**, so a double-run is caught, not just a miscount |
| A throwing task does not kill its worker | `ThreadPool.ExceptionsTravelInTheFuture…` | one worker only, so a dead worker **deadlocks** the test |
| All workers are used | `ThreadPool.UsesAllItsWorkers` | a barrier that cannot be satisfied sequentially |
| Concurrent submission | `ThreadPool.ConcurrentSubmissionIsSafe` | 8 threads × 50 tasks |
| Concurrent shutdown | `ThreadPool.ConcurrentShutdownCallsAreSafe` | 6 threads mixing `shutdown()` and `shutdownNow()` |
| `waitIdle()` cannot hang | `ThreadPool.WaitIdleDoesNotHangAfterShutdownNow` | run on another thread with a deadline, so a hang **fails** instead of wedging the suite |
| A token outlives its source | `Cancellation.TokenOutlivesItsSource` | the lifetime invariant the orphan path depends on |
| Cancellation wakes a sleeper | `Cancellation.WaitForWakesImmediatelyOnCancel` | |
| Signals are observable cross-thread | `Interrupt.IsVisibleFromAnotherThread` | raises a real signal |
| Concurrent DB writes serialise | `Repository.ConcurrentWritesAreSerialised` | 4 threads × 25 rows |

Ordering is asserted on **observable state**, not on sleeps, wherever it can be:
the `shutdownNow` discard test waits until `pool.stopping()` is true before
releasing its worker, rather than hoping a sleep is long enough.

---

## The flaky-test investigation

Two of the concurrency tests above were themselves flaky when first written —
1 to 2 failures per 5 full parallel runs. Shipping them would have been worse
than not writing them, so they were hunted:

```bash
for i in $(seq 1 5); do ctest --timeout 180 -j2 | grep -E "tests (passed|failed)"; done
```

| Test | Why it raced | Fix |
|---|---|---|
| `ThreadPool.ShutdownNowDiscardsQueuedWork` | released the blocking task *before* calling `shutdownNow()`; under load the single worker drained the whole queue in the gap | wait until `pool.stopping()` is observable, *then* release — ordering, not timing |
| `HttpServerLimits.TheCapIsNotAppliedToTrafficWithinIt` | used a cap of 1, which the server legitimately cannot honour back-to-back: a slot is released when the handler returns, possibly after the client already opened the next connection | raised the cap to 32, which is the property actually worth pinning |

The second is the more interesting one: the test was asserting a guarantee the
implementation deliberately does not make. **The right fix for a flaky test is
sometimes to correct the assertion, not the code** — but only once you can say
precisely why the original was wrong.

Confirmed with **six consecutive clean full parallel runs** afterwards. That is
the claim that can be supported; "no flaky tests" is not.

---

## CLI and REST smoke tests

Unit tests do not catch a binary that fails to start, a subcommand that was
never wired to its dispatcher, or a route that 404s. These are run manually as
part of release verification:

```bash
for c in version list "diagnose --no-color" "history --limit 3" stats "db info" "ai status"; do
  ./build/testforge $c > /dev/null && echo "$c ok"
done

./build/testforge spec path/to/spec.json        # both spellings must work
./build/testforge spec load path/to/spec.json
```

```bash
./build/testforge serve --port 8080 &
for p in /api/health /api/tests /api/runs / /nope /api/diagnostics; do
  curl -s -o /dev/null -w "$p %{http_code}\n" "http://127.0.0.1:8080$p"
done
curl -s -o /dev/null -w "chunked %{http_code}\n" -X POST \
     -H "Transfer-Encoding: chunked" -d '{}' http://127.0.0.1:8080/api/runs
kill -TERM %1     # must actually stop
```

Expected: six 200s, one 404, `400` for the chunked body, and `serve` exiting on
`SIGTERM`. That last one is not padding — the signal handlers were once
installed and never read, so `serve` could not be stopped at all.

---

## AI offline evaluation

The AI paths must be exercisable with no API key, or they are untestable in CI
and undemonstrable to a reader:

```bash
python -m python.evaluation.evaluate_generation --mock \
       --testforge ./build/testforge --output /tmp/eval.json
```

`MockAiProvider` returns deterministic specifications, so the whole
generate → validate → register → execute pipeline runs offline and produces
stable numbers. The harness scores *generation quality* — requirement coverage,
the ratio of negative cases, validation pass rate — not model quality.

The security boundary is checked the same way, by feeding the validator a
deliberately hostile specification and asserting each rejection:

```
rejected  tests[0].endpoint: absolute URLs are not allowed
rejected  tests[1].endpoint: must not contain '..'
rejected  tests[2].method:   'TRACE' is not allowed
rejected  tests[3].headers:  'Host' may not be set by a generated test
rejected  tests[3].headers:  'Authorization' may not be set by a generated test
```

**What this does not test** is the live OpenAI path. No request has ever been
sent from this project.

---

## What is deliberately not tested

| | Why |
|---|---|
| GPU code against real hardware | The development machine has none. The parser is tested against captured `nvidia-smi` output, the absence path is tested, and the limitation is stated. |
| Live model calls | Non-deterministic and costs money per run. The contract, the validator and the failure paths are tested against `MockAiProvider`; the sidecar's own schema validation is tested directly. |
| The dashboard's JavaScript | ~200 lines of DOM rendering with no logic worth asserting on. The API it consumes is tested. |
| Windows | The POSIX stubs compile and report features as unavailable; nothing is verified there. |
| Performance thresholds | A threshold that passes on a laptop and fails on a shared CI runner teaches people to ignore CI. The `perf` suite asserts only order-of-magnitude guard rails; real numbers come from `benchmarks/`, which reports rather than asserts. |

---

## Running everything

```bash
# the framework's own suite
ctest --test-dir build --output-on-failure

# one fixture, or one case
./build/testforge_tests --gtest_filter='SpecValidator.*'

# under sanitizers
scripts/build.sh --asan && ctest --test-dir build-asan --output-on-failure
scripts/build.sh --tsan && ctest --test-dir build-tsan --output-on-failure

# the bundled suites
./build/testforge run
./build/testforge run --suite failure_injection     # expected to exit 1

# the sample service's own suite
pytest sample-service/tests -q

# everything, in sequence
scripts/demo.sh
```

**Current status, measured on the development machine:**

| | |
|---|---|
| GoogleTest cases | 395 passing |
| Bundled tests | 59 across 6 suites |
| Sample-service pytest | 27 passing |
| Compiler warnings | 0, with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` and more |
