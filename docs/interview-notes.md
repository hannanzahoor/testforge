# Interview notes

An index of the concepts this codebase demonstrates, with pointers to where each
one actually lives.

This is **not** a tutorial. It is a map, written so that a specific technique can
be found and re-read in context later. Everything listed here is in the
repository; nothing is aspirational.

---

## C++ language and library

| Concept | Where |
|---|---|
| `std::variant` as a tagged union | `core/Json.hpp` — `Value` holds one of seven alternatives; `type()` derives from `index()` |
| Recursive types with incomplete elements | `Json.hpp` — `std::vector<Value>` inside `Value`, legal because vector permits an incomplete element type |
| Move semantics and the rule of five | `db::Connection`, `db::Statement`, `HttpClient` — movable, non-copyable |
| RAII for foreign resources | `db::Connection` (sqlite3*), `Statement` (sqlite3_stmt*), `Transaction` (rollback on scope exit), `Pipe` and `Socket` (file descriptors), `AddressList` (addrinfo) |
| Pimpl | `TestRunner`, `HttpClient`, `HttpServer`, `RestApiServer` — compilation firewall and stable ABI shape |
| Template metaprogramming | `detail::IsStreamable` — a `void_t` detection idiom that lets assertions print user types without requiring `operator<<` |
| `if constexpr` | `detail::display` — one function, six type categories, no overload set |
| Variadic templates + fold | `Statement::bindAll` — `(bind(++index, args), ...)` |
| Perfect forwarding | `ThreadPool::submit(Callable&&, Args&&...)` |
| `std::cmp_less` / `cmp_equal` (C++20) | `detail::lessValues` — mixed-sign comparison that is mathematically correct |
| Structured bindings | Throughout: `for (const auto& [key, value] : ...)` |
| `if` with initialiser | Throughout: `if (const json::Value* v = doc.find("k"); v != nullptr)` |
| `std::optional` | `parseUrl`, `tryParse`, `TestRegistry::find`, `loadRun` |
| `std::string_view` | Every read-only string parameter |
| `[[nodiscard]]`, `noexcept`, `constexpr` | Applied where they carry meaning, not uniformly |
| Function-local statics | `LogManager::instance()`, `TestRegistry::instance()` — thread-safe init, no static-order problem |
| `thread_local` | Log capture, soft-assertion scopes, worker names |
| Custom exception hierarchy | `core/Exceptions.hpp` — each type carries its own `FailureCategory` |
| Macros done carefully | `TESTFORGE_TEST_OPTS` — token pasting, `[[maybe_unused]]`, `do/while(false)` |

**Worth being able to explain:** why `std::vector<Value>` compiles inside
`Value`; why the JSON aliases moved *after* the class (enum members were
shadowing them under `-Wshadow`); why `Value(long)` uses braced init while
`Value(unsigned long)` uses `static_cast`.

---

## Object-oriented design

| Concept | Where |
|---|---|
| Interface segregation | `ResultRepository`, `DiagnosticProvider`, `AIProvider`, `Reporter`, `RunObserver`, `LogSink` — each small and single-purpose |
| Dependency inversion | The engine depends on those interfaces; `TestForgeService` injects the implementations |
| Polymorphism with a purpose | Every interface has ≥2 implementations *in-tree*, and the second one is usually what makes the first testable |
| Composition over inheritance | `ApiClient` is composed into a test rather than inherited from; `ApiTestCase` exists for the class-based case |
| The null-object pattern | `DisabledAiProvider`, `NullGpuProvider` — no call site needs a null check |
| Template method | `TestCase::setUp` / `execute` / `tearDown` |
| Strategy | Reporters, GPU providers, AI providers |
| Observer | `RunObserver`, so the runner streams progress without knowing what a terminal is |
| Factory | `TestFactory` in the registry — a fresh instance per execution |
| RAII scope guards | `ScopedLogCapture`, `SoftAssertionScope`, `Transaction` |
| Non-virtual public / virtual protected boundaries | `LogSink`, `DiagnosticProvider` — copy/move deleted on the base |

**Worth being able to explain:** why the registry stores factories rather than
instances (a retry must not inherit state); why `tearDown` is `noexcept` (a
throwing teardown masks the real failure).

---

## Data structures and STL

| Structure | Where and why |
|---|---|
| `std::map` | `TestRegistry` — ordered by qualified name, so listings are deterministic for free |
| `std::vector<pair>` for JSON objects | Insertion-ordered, so serialisation is byte-for-byte reproducible and diffable |
| `std::queue` behind a mutex | `ThreadPool` task queue |
| `std::set` | Deduplicating suites and tags |
| `std::unordered_map` avoided | Deliberately — ordering matters more than lookup speed at these sizes |
| Ring-buffer semantics | `MemoryLogSink` — keeps the tail, bounded memory |
| Nearest-rank percentiles | `RunStatistics` — returns an observed value, not an interpolated one |
| FNV-1a hashing | `ids::fnv1a64` — stable test ids and shard assignment |
| Iterative glob matching | `strings::globMatch` — linear, cannot blow the stack on `*a*a*a*b` |
| Recursive-descent parsing | The JSON parser, with an explicit depth cap |

---

## Concurrency

| Concept | Where |
|---|---|
| Worker pool | `ThreadPool` — fixed threads, shared queue |
| `std::mutex` + `condition_variable` | Queue and idle signalling |
| `std::future` / `std::promise` / `packaged_task` | Task results and exception propagation |
| `std::thread` + `std::promise` | The timeout helper thread — chosen over `std::async` because a promise-backed future has no blocking destructor, so an abandoned test can be walked away from without leaking or hanging |
| `std::atomic` with explicit ordering | `acquire`/`release` on cancellation; `relaxed` on counters |
| Cooperative cancellation | `CancellationToken` / `CancellationSource` |
| Async-signal-safe handling | `testforge::interrupt` sets an atomic; `serve` and `run` poll it, because a condition_variable wait is not woken by a signal |
| Lifetime under concurrency | `BodyExecution` in a `shared_ptr` so an abandoned thread cannot write to a freed stack frame |
| Lock scope discipline | The registry copies a factory out before invoking it; `LogManager` copies the sink list before writing |
| Thread-local state | Correct because a test runs on exactly one thread |
| Deterministic output from nondeterministic execution | Results re-sorted after the run |
| ThreadSanitizer and ASan/UBSan in CI | `.github/workflows/tests.yml`; both clean across 395 tests |

**Worth being able to explain:** why `pthread_cancel` is refused and what the
cost is; why a test finishing inside the grace period is still a `TIMEOUT`; the
release/acquire pairing on the cancellation flag.

---

## Linux and systems programming

| Concept | Where |
|---|---|
| `fork` + `execv` | `ProcessRunner` — argv array, no shell |
| Async-signal-safety between fork and exec | Only `dup2`, `close`, `open`, `chdir`, `execv`, `_exit` |
| Process groups | `setpgid(0,0)` so a timeout can signal descendants |
| `SIGTERM` then `SIGKILL` with a grace period | `ProcessRunner` |
| Pipes and non-blocking I/O | `Pipe`, `fcntl(O_NONBLOCK)`, `poll` |
| Zombie prevention | `waitpid` on every path |
| `/proc` and `/sys` parsing | `LinuxDiagnosticProvider` |
| `sysconf`, `statvfs`, `getifaddrs`, `uname` | Same |
| BSD sockets | `HttpClient` and `HttpServer` |
| Non-blocking connect + `poll` + `SO_ERROR` | `connectWithTimeout` |
| `TCP_NODELAY`, `SO_REUSEADDR`, `MSG_NOSIGNAL` | Set with a comment saying why |
| `EAGAIN` vs `EWOULDBLOCK` portability | `wouldBlock()` helper |
| Signal handling | `main.cpp` — a flag, plus `SIGPIPE` ignored |
| Container and WSL detection | `/.dockerenv`, `/proc/1/cgroup`, kernel strings |
| Running unprivileged | Diagnostics degrade rather than fail; the Docker image is non-root |

---

## Testing concepts

| Concept | Where |
|---|---|
| Test registry and discovery | Static registration at namespace scope |
| Fixtures and lifecycle | `setUp` / `execute` / `tearDown` |
| Assertion design | Expected/actual/location, not a bare boolean |
| Soft assertions | `SoftAssertionScope` |
| Failure taxonomy | 11 categories, two-pass classification |
| Test isolation | Fresh instance per execution |
| Skip vs fail | Missing hardware skips; a broken service fails |
| Boundary testing | Password length 7 and 8, both sides |
| Negative testing | Malformed bodies, wrong types, missing fields, duplicates |
| Failure injection | `examples/.../FailureInjectionTests.cpp` and `tests/failure_injection/` |
| Asserting the negative in CI | The injected suite must exit 1 with the right categories |
| Flake detection | Heuristic, labelled, plus the retry-passed signal |
| Test sharding | Stable, by hash of the test id |
| Seeded shuffling | Surfaces order dependencies reproducibly |
| Retry policy by category | Only transient categories are retried |
| Fakes over mocks | `InMemoryRepository`, `MockGpuProvider`, `MockAiProvider` |
| Sanitizers | ASan, UBSan, TSan in CI |

**Worth being able to explain:** why TestForge's own tests use GoogleTest rather
than TestForge; why the failure-injection suite is excluded from a default run.

---

## Networking and HTTP

| Concept | Where |
|---|---|
| HTTP/1.1 request and response framing | `HttpClient`, `HttpServer` |
| Chunked transfer decoding | `decodeChunked` |
| `Content-Length` vs EOF-terminated bodies | `HttpClient` |
| Redirect handling, including 303 → GET | `HttpClient::Impl::perform` |
| Header case-insensitivity and repetition | `HttpHeaders` |
| URL parsing: authority, IPv6, userinfo, query | `net/Url.cpp` |
| Percent encoding | `urlEncode` / `urlDecode` |
| Path-parameter routing | `HttpServer` — `/runs/:id` |
| 404 vs 405 | Tracked separately, because they mean different things |
| SSRF defence | `UrlPolicy` |
| Request/response size limits | Both directions |

---

## Databases

| Concept | Where |
|---|---|
| Repository pattern | `ResultRepository` |
| Prepared statements and binding | `db::Statement` |
| Transactions with RAII rollback | `db::Transaction` |
| Schema migrations | `persistence/Schema.cpp`, append-only |
| Index design for real queries | Six indices, each justified |
| Denormalisation for read performance | Counts on `test_runs` |
| Schemaless columns where appropriate | JSON columns with size caps |
| Referential integrity | `ON DELETE CASCADE` + `PRAGMA foreign_keys` |
| WAL for reader/writer concurrency | Connection setup |
| Durability trade-offs | `synchronous = NORMAL`, with the reasoning |
| Aggregate SQL | `GROUP BY`, `SUM(CASE)`, subqueries in `aggregateStatistics` |
| SQL injection defence | Parameters everywhere; tested with a real payload |

---

## Software architecture

| Concept | Where |
|---|---|
| Layering with a strict dependency direction | Core ← support ← engine ← service ← interfaces |
| One service layer, two front ends | `TestForgeService` — CLI and REST cannot drift |
| Configuration precedence | defaults → file → environment → flags |
| Structured logging | Fields, not prose; JSON sink for collectors |
| Observability | Run id correlation, per-test log tails, event timeline |
| Error-handling strategy | Test failure ≠ framework error ≠ configuration error, distinguished all the way to the exit code |
| Graceful degradation | No database, no GPU, no AI, no service — each degrades with a reason |
| Extension points | Six interfaces, each with a documented contract |

---

## AI engineering

| Concept | Where |
|---|---|
| Structured output | `response_format={"type":"json_object"}` plus schema validation |
| Validation as the boundary | `SpecValidator`, three times |
| A closed vocabulary as a safety property | `TestSpec` has no field for "run this" |
| Prompt-injection mitigation | Data in a user message; system prompt declares it untrusted |
| Advisory-only integration | The verdict is decided before the model is called |
| Provider abstraction | `AIProvider` with real, mock and disabled implementations |
| Deterministic offline mode | `MockAiProvider` — CI and demos need no key |
| Evidence engineering | `FailureContext` — allow-list, redaction, size budgets |
| Cost control | Caps on tests, characters, context and response |
| Evaluating generated output | `python/evaluation/` — six metrics, one of which is explicitly a heuristic |
| Secret isolation | The key lives in one process |

---

## Security

| Concept | Where |
|---|---|
| Command-injection defence | No shell, ever |
| SQL-injection defence | Parameters, always |
| SSRF defence | `UrlPolicy`, link-local blocking |
| Path traversal | Refused, not normalised |
| XSS in reports | HTML and XML escaping |
| Secret redaction | One choke point; idempotent |
| Allow-lists over deny-lists | Environment capture, AI metadata |
| Least privilege | Non-root container; diagnostics need no privileges |
| Defence in depth | Prompt + validator + closed interpreter |
| Documented threat model | `docs/security.md`, including what is *not* protected |

---

## Build and tooling

| Concept | Where |
|---|---|
| Modern CMake | Target-based, `PUBLIC`/`PRIVATE` correctness, INTERFACE targets for flags |
| Dependency acquisition | `FetchContent`, pinned, hash-verified, cached |
| OBJECT libraries | `examples/` — static registration survives linking |
| Generator expressions | `$<TARGET_OBJECTS:...>`, `$<BUILD_INTERFACE:...>` |
| Sanitizer configuration | Mutually exclusive combinations rejected at configure time |
| Warning discipline | Wide set, `-Werror` in CI, third-party code excluded |
| Multi-stage Docker | Build stage runs the tests; runtime stage carries only the binary |
| CI matrix design | Two compilers × two build types, `fail-fast: false` |
| Cache keying | Hash of the dependency file |

---

## Q&A

Answers grounded in this repository.

### Why C++?

Because the engine is the product, and its job is measurement. A test runner
that adds noticeable latency of its own corrupts the thing it is measuring; the
timing numbers it reports have to be about the system under test. C++ also
makes the interesting parts *mine* rather than a framework's: the scheduler, the
timeout mechanism, the cancellation model and the lifetime of an abandoned test
are all decisions I had to make and can defend, which is the point of the
project. And a single static binary with no runtime is what you actually want on
a test machine you did not provision.

### Why RAII?

Because this codebase holds a lot of things that are not memory: `sqlite3*`,
`sqlite3_stmt*`, file descriptors, pipe pairs, `addrinfo` lists, sockets,
threads. Every one of those has an early-return path in front of it —
`HttpClient::Impl::perform` alone has about a dozen — and hand-written cleanup on
each is how descriptors leak.

`FdGuard` in `HttpServer::handleConnection` is the clearest example: the
descriptor is owned by that function from the moment it enters, so every `return`
in the body, and every exception a handler throws, closes it.

### Why smart pointers, and which one where?

`unique_ptr` is the default: Pimpl in `TestRunner`, `HttpServer`, `HttpClient`,
and `TestContext` inside the execution bundle. Sole ownership, zero overhead,
and the destructor order is obvious.

`shared_ptr` appears in exactly two places, and both are cases where ownership is
genuinely shared rather than merely convenient:

- `CancellationToken::State` — the source is a stack object in `runOne`, the
  token may be held by a detached thread that outlives it. Shared state is what
  makes `Cancellation.TokenOutlivesItsSource` true.
- `BodyExecution` — co-owned by the worker and by the body thread, precisely so
  that abandoning the thread does not leave it writing into a dead frame.

Where a `shared_ptr` would only mean "I have not thought about ownership", there
isn't one.

### Why a worker pool rather than a thread per test?

A 10,000-test run should cost N threads, not 10,000. Thread creation is not free
and the scheduler does not thank you for oversubscription. The pool creates its
workers once in the constructor and joins them in the destructor.

One wrinkle: a test *with a timeout* still gets one helper thread, because
the worker has to be able to stop waiting at the deadline. So `--workers 4` means
four workers plus up to four short-lived helpers, and the workers are blocked
while their helpers run. That is the price of not killing threads.

### Why `condition_variable`?

Because the alternative is a spin loop, and idle workers must cost nothing. The
pool uses two: `workAvailable_` parks workers until something is queued, and
`idle_` wakes `waitIdle()` when the pending count reaches zero.

Both waits use the predicate overload, which handles spurious wake-ups. And
`idle_` is why the discard path in `shutdownNow()` has to decrement `pending_`
for every dropped task — miss one and `waitIdle()` does not return a wrong
answer, it hangs.

### How does cancellation work?

A `CancellationSource` (writable) and `CancellationToken` (read-only) share a
`shared_ptr<State>` holding an atomic flag, a mutex, a condition variable, a
reason string and an optional deadline. `cancel()` mutates under the lock,
stores the flag with release ordering, then notifies after unlocking. The first
reason wins, so a later blanket "run aborted" cannot overwrite the more specific
"deadline exceeded".

A test cooperates in three ways: `ctx.throwIfCancelled()` between steps,
`ctx.sleepFor()` instead of `std::this_thread::sleep_for`, and passing the token
to the HTTP client, which aborts long reads.

Run-level cancellation reaches a running test in two ways, and it needed both.
Each executing test publishes its source in a registry that
`requestCancellation()` signals directly — without that, a test with **no
timeout** runs inline on the worker and nothing is polling on its behalf, so it
could not observe Ctrl-C at all. The timed path also forwards the run source
inside its polling loop, as a backstop for fail-fast.

### Why cooperative cancellation rather than killing the thread?

`pthread_cancel` and its relatives leave mutexes locked, destructors unrun and
the allocator in an unknown state. Every verdict the process reports *afterwards*
becomes suspect.

For a tool whose entire product is trustworthy verdicts, that trade is not worth
making. So a timed-out test is asked to stop; if it refuses, the runner stops
waiting, records `TIMEOUT`, detaches the thread and names it at shutdown:

```
WARN [runner] tests ignored cancellation and are still running at shutdown
     count=1 tests=failure_injection.uncooperative_timeout
```

The cost is stated rather than hidden: an uncooperative test occupies a thread
until the process exits.

### How does `shutdownNow()` work?

It sets `stopping_` and `discardQueued_` under the queue mutex, notifies every
worker, then joins. A worker that wakes and sees both flags drains the queue —
popping each task **and decrementing `pending_`** — notifies `idle_`, and
returns.

That decrement is the whole subtlety. `pending_` is what `waitIdle()` blocks on,
so an accounting slip there is a hang rather than a wrong answer. A discarded
task's `packaged_task` is destroyed unrun, so anyone holding its future gets
`future_error(broken_promise)` instead of waiting forever.

None of this was covered by a test until the audit; there are now nine.

### What race conditions were found?

Three, all real:

1. **`HttpServer::stop()` vs `acceptLoop()`.** `stop()` closed the listening
   descriptor *before* joining the acceptor, so the acceptor could `poll()` or
   `accept()` on a closed fd — and on a busy process that number may already
   name a different file. Found by ThreadSanitizer. Fixed by joining first.
2. **Two concurrent `shutdown()` callers** would both iterate `workers_` and
   `join()` the same `std::thread`, which is undefined, while one cleared the
   vector the other was walking. Found by reading, not by a tool: the header
   claimed a second call was safe, which was only true sequentially.
3. **The use-after-free** below — a lifetime bug rather than a data race, but
   the same family.

### How did ThreadSanitizer help, specifically?

The first run produced seven "double lock of a mutex" reports in the
cancellation path. The two easy responses — ignore TSan, or suppress the whole
file — would both have been wrong.

Reading the full report showed the mutex was "created at" a `lock()` call, which
is impossible unless TSan never saw an initialisation. libstdc++ on Linux
initialises `std::mutex` statically with `PTHREAD_MUTEX_INITIALIZER`, so there is
no `pthread_mutex_init` for TSan to observe, and combined with the internal
unlock/relock in `pthread_cond_timedwait` its ownership bookkeeping goes wrong.

That diagnosis justified suppressing **two function names**, not a file and not a
category. With the noise gone, TSan immediately reported the genuine
`HttpServer::stop()` race that had been buried underneath. **Narrowing the
suppression is what found the bug.**

### How was the use-after-free discovered?

Not by a sanitizer. The suite was green under ASan.

It came from reading `TestContext` next to the orphan path. `TestContext` holds
non-owning pointers to its `Config` and `TestMetadata`. The `BodyExecution`
bundle exists precisely so those stay valid for an abandoned thread — and the
metadata was an owned copy inside it, but the **Config was still a reference into
the caller's storage**. So a stray test that outlived the reaper's grace period
and then touched `ctx.config()` would read freed memory.

It was *confirmed* by writing the regression test first, then reverting the fix
and rebuilding the same ASan binary with one line changed:

```
WITH the fix:     [  PASSED  ] 1 test.
WITHOUT the fix:  ERROR: AddressSanitizer: heap-use-after-free
  READ  #4 RunnerTest.cpp (the orphan reading ctx.config())
        #11 TestRunner.cpp (the detached body thread)
  freed by T0  #4 RunnerTest.cpp (ownedConfig.reset())
```

The fix is one line of intent: the Config joins the bundle as an owned copy.

### Why SQLite?

Because the data is small, local, and relational, and the alternative is a
service to install before you can run a test. SQLite is a file — it ships
vendored as the amalgamation with a pinned SHA-256, so there is no
`libsqlite3-dev` to apt-get and the build works on a bare machine.

WAL mode is the specific reason it fits: the dashboard and `testforge history`
can read while a run is writing. Where it would *not* fit is many machines
writing one database, which is exactly what a real CI fleet does — that is a
stated limitation, not a claim.

### How is SQL injection prevented?

Every user value is bound. The only string concatenation in the query layer is a
compile-time column-list constant and fixed `WHERE` fragments chosen by
booleans:

```cpp
std::string sql = "SELECT " + std::string(kRunColumns) + " FROM test_runs WHERE 1 = 1";
if (query.labelContains.has_value()) { sql += " AND label LIKE ?"; }
...
statement.bind(++index, "%" + *query.labelContains + "%");
```

The clause shapes are decided by the code; the values never touch the SQL text.
One nuance worth volunteering: the `LIKE` pattern has no `ESCAPE` clause, so `%`
and `_` inside a user's label filter act as wildcards. That broadens a filter; it
does not inject.

### How does API test generation work?

`testforge ai generate-tests --requirement "..."` goes to
`TestForgeService::generateTests`, which asks the configured `AiProvider`. In
practice that is `HttpAiProvider`, which POSTs to the Python sidecar — pinned by
`UrlPolicy::restrictedTo(endpoint)` with redirects disabled. The sidecar calls
OpenAI with `response_format={"type":"json_object"}` and returns a specification.

The engine then validates it, registers the survivors as `SpecTestCase`s, and can
execute them in the same process — registration is in-memory, so a separate
`testforge run` would find nothing.

Offline, `MockAiProvider` returns deterministic specifications so the whole
pipeline is demonstrable with no key. **That is the only path that has actually
been exercised**; no request has ever been sent to OpenAI from this project.

### Why does AI not determine PASS/FAIL?

Because a verdict has to be reproducible and attributable, and a model's output
is neither. If the model decided, then the same code and the same service could
produce different results on Tuesday, and there would be no way to say why a
build went red.

So the split is: the model may **propose** tests and **explain** failures; the
C++ engine decides. Every AI-produced field in a report is labelled advisory, and
`TestRun::exitCode()` never reads one.

### How does the AI security boundary work?

Four layers, and the important property is that the last two do not depend on
the model behaving:

1. **Framing.** Untrusted text travels in a *user* message as a JSON envelope,
   never concatenated into the system prompt, and the system prompt states that
   the payload is data.
2. **Shape.** `response_format=json_object` constrains syntax — but only syntax.
3. **`SpecValidator`.** Allow-listed methods (no `TRACE`, no `CONNECT`),
   allow-listed assertion kinds, relative endpoints only (no scheme, no `//`, no
   `..`, no `@`, no control characters — CR/LF there is request splitting), a
   deny-list for routing and credential headers, then the endpoint is resolved
   against the base URL and re-checked under the URL policy. Caps on name
   length, body size, header count and assertions per test.
4. **Re-validation at registration.** `registerSpecSuite` runs the same
   validator again rather than trusting its caller.

Fed a deliberately hostile specification, all four entries are rejected with
their reasons.

### How does GPU detection work?

`NvidiaGpuProvider::isAvailable()` looks for `nvidia-smi` on `PATH`. If present,
it runs it — via `ProcessRunner`, so `execv` with a fixed argument vector, no
shell — asking for eleven named fields as `csv,noheader,nounits`, plus
`--version` and `--query-compute-apps`.

Failure modes are handled individually rather than collapsed: not found, launch
error, timeout, non-zero exit (usually a driver/library version mismatch), and
ran-but-reported-no-devices each produce their own `unavailableReason`. Fields a
card does not expose (`[N/A]`, `[Not Supported]`) become **-1, meaning unknown**,
never 0 — otherwise a report would claim a GPU is at 0 °C.

The parser is a pure static function, which is what makes it testable with no
hardware: it is exercised against captured real output (A100 and T400 rows) and
against malformed rows. **No GPU has ever been present on this machine**, so what
has never run is the subprocess call itself. `MockGpuProvider` exists only for
the test suite and is not wired into any production path.

### Why is the project Linux-first?

Because the diagnostics *are* the differentiator, and they are `/proc`, `/sys`,
`getifaddrs`, `statvfs` and `uname`. A portable abstraction over those would
either be a lowest common denominator or three implementations, only one of
which I could test.

The subprocess model is equally Linux-shaped: `fork` + `execv` with
`setpgid(0,0)` so a timeout can kill the whole process group.

The Windows build compiles, and that is all it does — the HTTP client, HTTP
server, `ProcessRunner` and diagnostics are empty stubs. Calling that
"cross-platform" would be a lie; it is a compile target.

### What are the known limitations?

The full list is [limitations.md](limitations.md). The ones I would raise
unprompted:

- **Docker has never been built.** No daemon was available; the configuration
  parses and nothing more.
- **GitHub Actions has never run on GitHub.** Every command in the workflows
  passes locally; the workflows themselves are unverified.
- **No real GPU, no real OpenAI call.** Both paths are implemented; only the
  mock paths have been exercised.
- **No TLS**, so the HTTP client refuses `https://` outright rather than failing
  confusingly.
- **The HTTP server is minimal**: `Connection: close`, no keep-alive, no chunked
  request bodies, no authentication.
- **Coverage is not measured**, so I do not quote a percentage.
- **Two TSan suppressions** are in force, scoped to two function names, for a
  false positive I can explain. No data race is suppressed.
- **No AI agent and no tool-calling** — one request, one JSON response.

---

## Questions this codebase can answer

Not a quiz — the places where a specific, defensible decision was made and can
be discussed:

1. Why does a timeout not kill the thread, and what does that cost?
2. Why does the registry hold factories instead of instances?
3. How can an abandoned thread write to a `TestResult` safely?
4. Why is `tearDown` `noexcept`?
5. Why does a failed assertion against a 503 become an application failure, but
   against a 404 stay an assertion failure?
6. Why does the success rate exclude skipped tests?
7. Why can't a language model change a test verdict, structurally rather than by
   policy?
8. Why is a generated test a data structure rather than code?
9. Why does redaction have to be idempotent? (There is a real stack-overflow
   story here.)
10. Why is `ASSERT_*` opt-in?
11. Why does CI assert that a suite *fails*?
12. Why parse `/proc` instead of running `free`?
13. Why is "unknown" different from zero in a diagnostics payload?
14. Why is the flake detector called a heuristic in the payload itself?
15. Why is there no PostgreSQL implementation, given the interface exists?

---

## Real bugs found and fixed during development

Worth more than the feature list, because they are what the tests were for:

| Bug | Found by | Fix |
|---|---|---|
| `redactSecrets` recursed on its own mask → infinite recursion → stack overflow → segfault | `smoke.string_utilities` crashing the run | Rewritten iteratively and idempotently; `Strings.RedactionIsIdempotent` pins it |
| An uncooperative test that finished during the grace period was reported `PASSED` | The failure-injection suite reporting the wrong outcome | Deadline tracked separately; `FinishingInsideTheGracePeriodIsStillATimeout` pins it |
| `ConsoleReporter` dereferenced a missing `http_method` → segfault | `InjectionFixture.FailuresRenderInEveryReportFormat` | Safe JSON readers (`json::stringAt`, etc.); 110 call sites swept |
| A multi-line assertion message broke the console table and the DB column | Visible in the injected suite's output | Message is now the first line; the rest goes to detail |
| Exit code 3 for a test that hit a configuration error | Demo showing the wrong code | `exitCode()` reserves 2 for `FRAMEWORK_ERROR` only |
| The response size cap was applied while reading headers | `HttpFixture.ResponseSizeIsCapped` | Separate header and body limits |
| A `TestResult&` bound into a destroyed temporary | `RunnerFixture` reading empty metadata | Named local for the `TestRun` |
| The body cap rejected large responses instead of truncating them | Same test | Header cap 64 KiB, body cap configurable |
| `HttpServer::stop()` closed the listening socket *before* joining the acceptor, so the acceptor could `poll()`/`accept()` a closed — and possibly reused — descriptor | **ThreadSanitizer**, once the mutex noise was suppressed | Join the acceptor first, close afterwards; the fd then has exactly one owner at a time |
| 1743 bytes leaked in 15 allocations, from `std::future`s deliberately leaked to avoid `std::async`'s blocking destructor | **LeakSanitizer** (`ASAN_OPTIONS=detect_leaks=1`) | Redesigned the mechanism rather than suppressing the report: `std::thread` + `std::promise` + `detach()` leaks nothing |
| The `secrets` CI job would have failed on every run: the redaction tests necessarily contain secret-shaped fixtures | Running the shipped scan locally during final verification | Per-line `NOT-A-SECRET` markers (11 of them) rather than excluding `tests/` — a directory exclusion would also hide a real key committed in a test. The job prints the exemption count so growth is visible |
| `ruff format --check` failed on 6 files, and `ruff check` found an unused import — both blocking CI steps | Same pass; the tooling had never been run locally | Fixed the code, then added `pyproject.toml` and pinned ruff in the workflow |
| SIGINT/SIGTERM handlers were installed but the flag was never read anywhere, so Ctrl-C and `docker stop` could not stop `testforge serve` at all | Trying to kill a leftover server during final verification | A `testforge::interrupt` module the rest of the code can actually see, plus polling in `serve` and a watcher thread in `run`; 6 tests raise real signals |
| Once interrupts worked, cancellation was reported as test *failures*: cancellation-aware sleeps returned early and the following assertions failed | Interrupting a real run and reading the output | A test cancelled by a run-level cancel is recorded Skipped, not Failed — its verdict is an artefact of the cancellation |
| Then an interrupted run reported `RESULT: PASS` and exit `0`, claiming success for tests it never ran | Same session; nothing "failed", so the exit code was clean | `exitCode()` returns 4 when cancelled, the CLI maps it to `128 + signal`, and the console says `RESULT: INCOMPLETE`. A real failure still outranks it |
| `TestContext` held a raw `const Config*` into storage the runner did not own, so an orphaned test outliving the reaper's grace period read freed memory | A read-only audit, then proved by writing the regression test and reverting the fix: ASan reports `heap-use-after-free` in the detached body thread | The Config joins `BodyExecution` as an owned copy, alongside the metadata that was moved there for exactly this reason |
| A test with no timeout could not observe Ctrl-C at all: it runs inline on the worker, and only the *timed* path polled the run-level cancellation source | Audit, reading the two execution paths side by side | Each running test publishes its CancellationSource in a registry that `requestCancellation()` signals directly; the polling loop keeps its forward as a backstop for fail-fast |
| `Authorization` and `Cookie` were forwarded across a cross-origin redirect — the leak curl gates behind `--location-trusted` | Audit of the redirect path | Credentials dropped on any change of scheme, host or port, and never restored if a later hop returns home |
| Every root-relative redirect went to the wrong path: `Location: /landing` from `/same-origin` resolved to `/same-origin/landing` | The redirect test written for the credential fix failed with a 404 | `joinUrl()` appends because its job is base + endpoint; resolving a `Location` replaces the path per RFC 3986. The redirect path now does the latter |
| Two threads in `ThreadPool::joinAll()` would both `join()` the same `std::thread` and mutate `workers_` while the other iterated it | Audit; the header claimed "a second call is safe", which was true only sequentially | A join mutex plus an idempotence flag; `workers_` is no longer cleared, which also makes `workerCount()` a race-free read |
| `CancellationToken.cpp` called `std::this_thread::sleep_for` without including `<thread>` | Audit; it compiled only because libstdc++ pulls it in via `<condition_variable>` | Added the include |
| 110 of 120 files did not match the committed `.clang-format`, and the job that checks it is blocking | Installing clang-format and actually running it | Whole tree reformatted; the exact CI command now exits 0 |
| The lint result depended on which ruff was installed — 1 finding under 0.12, 113 under 0.16 — because CI ran `pip install ruff` unpinned with no config file | Comparing the two versions deliberately | `pyproject.toml` fixes `target-version` and the rule selection; the workflow pins the same range as `requirements.txt`. Verified clean under both |
