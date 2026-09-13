# Design decisions

Every entry is a choice that could reasonably have gone the other way, with the
reasoning and the cost of being wrong.

---

## 1. C++ is the engine; Python is used where Python is better

**Decision.** The registry, selector, runner, thread pool, assertion engine,
failure classifier, HTTP client and server, diagnostics, persistence, reporting
and CLI are all C++. Python owns the sample service, the model orchestration and
the AI evaluation harness.

**Why.** These are two different kinds of work. The engine is long-lived,
concurrent, and has to be trustworthy about time and memory — that is C++'s
argument. The sidecar is glue around a vendor SDK whose retry and rate-limit
behaviour is already written in Python, and prompts change far more often than a
test runner does. Putting the sidecar in C++ would mean reimplementing an SDK
and recompiling to edit a prompt.

**The line.** Python is never in the path of deciding whether a test passed.
Remove the entire `python/` directory and TestForge still builds, runs, reports
and persists; only the AI features report themselves as unavailable.

---

## 2. A hand-written JSON library instead of nlohmann/json

**Decision.** `core/Json.hpp` — about 900 lines including the parser, serialiser
and safe readers, with 40 unit tests.

**Why.** Two reasons, one practical and one about scope.

The practical one: TestForge has to build on a machine with no development
headers installed. Adding a header-only JSON library is easy, but it is one more
thing to fetch and one more version to pin, and JSON is the *only* thing it
would be used for.

The scope one: JSON is on the path of everything untrusted here — config files,
HTTP responses, model output, database columns. Owning the parser means owning
its limits: a depth cap against stack exhaustion, a length cap, exact
round-tripping of 64-bit integers so a test id is not silently degraded through
a double, and insertion-ordered objects so report output is byte-for-byte
reproducible and therefore diffable.

**Cost if wrong.** A parser bug would be silent and everywhere. Mitigated by
testing the awkward cases specifically: surrogate pairs, lone surrogates, deep
nesting, precision round-trips, duplicate keys, unescaped control characters,
and a set of fourteen malformed documents that must be rejected.

---

## 3. A hand-written HTTP client instead of libcurl

**Decision.** `net/HttpClient` — non-blocking connect with `poll`, deadline
enforcement, chunked decoding, redirects, cooperative cancellation. No TLS.

**Why.** `libcurl4-openssl-dev` is not installed on every machine, and the
features TestForge actually needs are a few hundred lines. Writing it also buys
something libcurl would not give easily: the client honours a
`CancellationToken`, so a timed-out test stops waiting on a socket instead of
blocking its worker until the OS-level timeout.

**The explicit cost.** No HTTPS. An `https://` URL returns a transport error
saying exactly that and pointing at the sidecar, rather than failing a handshake
in a way nobody can read. For testing a service you are developing — the actual
use case — plain HTTP on loopback is what you have anyway.

---

## 4. Timeouts use cooperative cancellation, never thread termination

**Decision.** A timed-out test is signalled through a `CancellationToken`. If it
does not stop, the runner records `TIMEOUT`, releases the worker, and abandons
the thread. `pthread_cancel` is never called.

**Why.** Forcibly terminating a thread leaves mutexes locked, destructors unrun
and the allocator in an unknown state. Everything the process reports afterwards
becomes suspect — in a tool whose entire product is trustworthy verdicts, that
is the worst possible trade.

**The cost.** An uncooperative test leaks a thread until the process
exits. TestForge does not hide this: the result explains what happened and how
to fix the test, and the runner names the stray threads at shutdown. Full
discussion in [concurrency.md](concurrency.md).

**Rejected alternative.** Running each test in a child process would allow a
hard kill. It also means serialising results across a process boundary, losing
in-process registration, and paying a fork per test. Wrong trade for this scale.

---

## 5. SQLite, behind a repository interface

**Decision.** `ResultRepository` is the interface the engine sees.
`SqliteResultRepository` is the only real implementation;
`InMemoryRepository` (in `tests/fixtures`) is the other.

**Why SQLite.** A test tool should not require a server to be running before it
can record a result. The database is a file: it travels with the workspace, CI
can upload it as an artefact, and there is nothing to provision.

**Why the interface.** Not speculative generality — the second implementation
already exists and earns its place. `tests/integration/RunnerTest.cpp` exercises
retries, fail-fast, persistence ordering and diagnostics attachment against the
in-memory fake, with no schema and no file system. A PostgreSQL implementation
would be a new class, not a refactor.

**Not doing.** PostgreSQL and MongoDB are documented as possible extensions and
deliberately not implemented. Adding a datastore with no use case would be
technology for its own sake.

---

## 6. A worker pool, not a thread per test

**Decision.** `ThreadPool` with N workers and a thread-safe queue.

**Why.** A thread per test means a 10,000-test run creates 10,000 threads. The
pool bounds concurrency to something the machine can actually schedule, and
worker identity (`worker-3`) lands on every result, which is genuinely useful
when a failure only happens under contention.

**The exception.** When a test has a timeout, its body runs on a short-lived
helper thread so the worker can stop waiting at the deadline without killing
anything. That is one extra thread per *timed* test, sequentially — the pool
still bounds how many run at once.

---

## 7. Diagnostics are collected only for failures

**Decision.** `collectOnFailure` defaults to true; passing tests get nothing.

**Why.** Reading `/proc`, sampling CPU over a window and invoking `nvidia-smi`
costs 100–300 ms. For a suite of 500 fast tests that is minutes of pure
overhead, and the diagnostics for a passing test are never read.

The failure payload is also trimmed relative to `testforge diagnose`: only
filesystems under pressure, only interfaces that are up. It is stored next to
every failing result, so its size is multiplied by the number of failures.

---

## 8. The AI decides nothing

**Decision.** Two AI features, both gated. Generation produces a *declarative
specification* that `SpecValidator` must accept. Analysis produces an
*explanation* labelled `advisory_only` and rendered next to the engine's own
verdict.

**Why generation is a specification and not code.** A generated test can express
a method, a relative path, headers, a body, an expected status and assertions
from a closed list of nine kinds. There is no field for "run this command", so
there is no path from model output to arbitrary behaviour. The safety comes from
the *shape of the data type*, not from the prompt.

**Why analysis cannot change a verdict.** A model that could flip a failure to a
pass would make every green run unfalsifiable. Pass and fail are decided by
assertions before the model is called, and every analysis payload carries the
engine's verdict alongside its own opinion so no consumer can render one without
the other.

**Validation runs three times** — sidecar, `SpecValidator`, then again in
`registerSpecSuite` — because the second one is the boundary and a caller that
skipped it must not be able to register anything runnable.

---

## 9. The OpenAI key never enters the C++ process

**Decision.** The key lives in the Python sidecar. The engine talks to the
sidecar over plain HTTP on loopback.

**Why.** Three things fall out of it: the credential exists in exactly one
process; the C++ side needs no TLS stack; and prompts can be edited without a
recompile. It also makes "no key configured" a clean, testable state rather than
an error path scattered through the engine.

**Cost.** One more process to run for live AI. Mitigated by `MockAiProvider`,
which makes the offline path a first-class, deterministic feature rather than a
degraded one.

---

## 10. CMake FetchContent for dependencies

**Decision.** FetchContent, pinned by version, with SQLite pinned by SHA-256.

**Why.** vcpkg and Conan are better at large dependency graphs. This graph has
two entries, one of which is only needed to build the tests. FetchContent means
`cmake -S . -B build` works on a clean checkout with nothing installed first,
which is the property that matters for something a stranger might clone.

Archives are cached under `.deps-cache/`, so CI restores them and offline
rebuilds work.

---

## 11. `TF_ASSERT_*` is the real name; `ASSERT_*` is opt-in

**Decision.** `Assertions.hpp` defines `TF_ASSERT_TRUE` and friends.
`ShortAssertions.hpp` adds the unprefixed spellings, and `#error`s if GoogleTest
is already included.

**Why.** `ASSERT_*` is GoogleTest's namespace. Silently shadowing it would
produce a baffling error the first time someone included both headers.
TestForge's example suites include the short header; TestForge's own
GoogleTest-based tests do not — which is also the right separation on principle,
since the thing under test should not be the thing doing the checking.

---

## 12. A single-file dashboard with no build step

**Decision.** One HTML file, plain JavaScript, served by the C++ server.

**Why.** A React + Vite dashboard needs node, a package install, a bundler and a
lockfile to display four numbers and a table. The engineering value here is in
the engine, and the dashboard's job is to make the engine's output visible.
Zero build means it works from a clone with no toolchain, and there is no
`node_modules` to age.

**Cost.** It will not scale to a complex UI. If it needed one, that would be the
moment to introduce a framework — not before.

---

## 13. The HTTP server is C++, in-process

**Decision.** `net/HttpServer` — HTTP/1.1, `Connection: close`, worker pool,
request limits, path-parameter routing.

**Why.** A Python REST layer would need to reach the registry and the runner
somehow: either shelling out to the CLI (losing in-process state, paying a
process launch per request) or a second IPC mechanism. Keeping the server in the
same process means `POST /api/runs` and `testforge run` take exactly the same
code path through `TestForgeService`, so the two interfaces cannot drift.

**Scope, stated in the header.** No keep-alive, no TLS, no HTTP/2, no
authentication. Loopback by default. It is a developer and CI tool.

---

## 14. Unknown CLI options are an error

**Decision.** `--sutie smoke` exits 2 with a message, rather than being ignored.

**Why.** A misspelled filter that is silently dropped runs the *entire* suite
while the user believes they ran one test. On a slow suite that wastes an hour;
in a pre-merge check it produces a false sense of coverage.

---

## 15. Exit codes distinguish four kinds of outcome

**Decision.** `0` passed, `1` tests failed, `2` bad usage or configuration,
`3` TestForge itself failed, `128 + signal` interrupted before finishing.

**Why.** CI needs to treat them differently. "Some tests failed" is a normal
result to report to a developer; "the tool is broken" should page whoever owns
the tool. `TestRun::exitCode()` reserves `2` for `FRAMEWORK_ERROR` only — a
*test* that hits a configuration problem is still a test result, and reporting
it as a tool failure would send CI looking in the wrong place.

**Why an interrupted run is not `0`.** It is the case most easily got wrong.
When a run is cancelled part-way, nothing has *failed* — so the obvious
implementation returns `0`, and CI goes green on a run that verified almost
nothing. `TestRun::exitCode()` therefore returns `4` for a cancelled run, which
the CLI maps to the conventional `128 + signal`. A genuine failure still wins:
a run that both failed a test and was then interrupted exits `1`, because that
is the more actionable fact.

---

## 16. The failure-injection suite is excluded from a bare `run`

**Decision.** `execution.exclude_suites_by_default` lists `failure_injection`.
Naming any filter explicitly overrides the exclusion.

**Why.** Those tests are supposed to fail. Including them by default would make
a healthy checkout look broken, and people would learn to ignore a red run —
which is the single worst thing that can happen to a test suite.

**Why config and not a hardcoded rule.** It is discoverable, it is overridable,
and it does not require a special-case in the selector.

---

## 17. Reports are pure functions of a `TestRun`

**Decision.** A `Reporter` takes a `TestRun` and returns a string. It never
queries the database or the network.

**Why.** Every reporter becomes trivially testable against a hand-built
`TestRun`, and `testforge report --run <id> --format html` can re-render an old
run from stored data without re-executing anything.

---

## 18. Diagnostics read `/proc` directly instead of shelling out

**Decision.** `/proc/meminfo`, `/proc/stat`, `/proc/mounts`, `statvfs`,
`getifaddrs`. Not `free`, `df`, `top`.

**Why.** Faster; independent of which coreutils flavour is installed;
locale-proof; and it removes an entire class of command-injection risk by not
building command lines at all.

`journalctl` and `dmesg` are the exceptions — there is no file equivalent — and
both go through `ProcessRunner` with a fixed argument vector, both off by
default because they usually need privileges.

---

## 19. Unknown diagnostic values are absent, not zero

**Decision.** Every numeric field defaults to `-1` and is omitted from JSON when
unknown. Failures to read are collected into an `unavailable` list.

**Why.** "0 MB of GPU memory" and "we could not read GPU memory" are completely
different findings, and a report that renders them identically will mislead
somebody at 2am.

---

## 20. Redaction happens at the sink, not at the call site

**Decision.** `Logger::log` passes every message and every string field through
`redactSecrets` before it reaches a sink. `HttpHeaders::toRedactedJson`,
`env::snapshotRedacted` and `FailureContext` do the same on their paths.

**Why.** Relying on every call site to remember is relying on the one that
forgets. One choke point is auditable.

**Known limit.** It recognises common shapes — `Bearer` tokens, `sk-`/`ghp_`
prefixes, sensitive key names — and nothing else. It is defence in depth, not a
guarantee, and [security.md](security.md) says so.

---

## Rejected

| Considered | Why not |
|---|---|
| A process per test | Result serialisation, lost in-process registration, a fork per test. Wrong at this scale. |
| gRPC between engine and sidecar | Protobuf toolchain and codegen for two endpoints on loopback. |
| Kafka / a message bus for results | There is one producer and one consumer, in the same process. |
| MongoDB alongside SQLite | No use case. Two datastores for one workload is technology for its own sake. |
| A plugin ABI for test suites | Static registration into one binary is simpler, faster and easier to debug. |
| Distributed execution | Useful, large, and out of scope here. Sharding (`--shards`/`--shard`) covers the common case by letting CI run N independent copies. |
