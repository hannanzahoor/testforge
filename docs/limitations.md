# Limitations and Scope

The boundaries of what TestForge does, what it guarantees, and which parts
depend on an environment that was not available during development.

This document exists so that the README can stay focused. Nothing here is
hidden elsewhere.

---

## Scope

TestForge is a developer and CI tool, built to production coding standards but
without the operational history that comes from running somewhere for years. It
is an independent project and does not reproduce any organisation's internal
test infrastructure.

---

## Platform

**Linux-first.** Developed and verified on Ubuntu 22.04, including under WSL2.

The POSIX code paths — `ProcessRunner` (fork/exec/poll), `HttpClient` and
`HttpServer` (BSD sockets), the `/proc` diagnostics — require a POSIX system.
There is a `#if defined(_WIN32)` stub for each that compiles and reports the
feature as unavailable, rather than pretending. Nothing on Windows has been
verified beyond compilation.

macOS would probably work for most of it — the socket and process code is
POSIX — but `/proc` does not exist there, so the Linux diagnostics would report
almost everything as unavailable. Untested.

**Not verified:** 32-bit builds, big-endian architectures, musl libc, or any
compiler other than GCC 11 and Clang 14.

---

## Concurrency

**An uncooperative test leaks a thread until the process exits.** This is the
deliberate consequence of refusing to call `pthread_cancel` — see
[concurrency.md](concurrency.md#orphaned-tests). A test that ignores
cancellation and blocks forever will hold one thread for the life of the
process. TestForge records the result as `TIMEOUT`, releases the worker, and
names the stray thread at shutdown, but it cannot reclaim it.

**No cross-process concurrency limit.** Two `testforge` processes each with
`--workers 8` will use 16 workers.

**Single machine.** `--shards N --shard I` lets a CI matrix run N independent
copies, but nothing coordinates them: no shared queue, no work stealing, no
central result store. Distributed execution is genuinely useful and genuinely
out of scope for this project.

**Test isolation is not enforced.** Tests share a process. TestForge gives each
a fresh instance, its own context and its own log capture, but it cannot stop
one from writing to a shared file, mutating a global, or leaving the system under
test in a different state. `--shuffle --seed N` surfaces order dependencies; it
does not prevent them.

---

## Network

**No TLS in the C++ client.** `HttpClient` links no TLS stack. An `https://` URL
returns a transport error saying exactly that. Anything needing TLS — the model
provider — goes through the Python sidecar.

**No connection reuse.** Every request opens and closes a socket. Fine for test
workloads; it would matter for a benchmark of a service's peak throughput.

**No HTTP/2, no compression, no cookies.** None of them are needed by the test
workloads, and each would be code with no test to justify it.

**The URL policy does not defend against DNS rebinding.** It checks the literal
host in the URL and does not re-resolve after connecting. For a tool pointed at
a service you control, this is an acceptable gap; it is recorded here rather
than glossed over.

**The HTTP server is minimal.** HTTP/1.1 with `Connection: close`, one request
per connection, no keep-alive, no TLS, no HTTP/2, no authentication. It is a
developer and CI tool.

---

## GPU

**Not verified against physical NVIDIA hardware.** The development machine has
none.

| | |
|---|---|
| Verified | CSV parsing against captured real `nvidia-smi` output, including multi-GPU rows and `[N/A]` fields |
| Verified | The absence path, on a machine with no GPU |
| Verified | Mock labelling end to end |
| **Not verified** | Execution against a real GPU |

The sample output in [gpu-diagnostics.md](gpu-diagnostics.md) uses realistic
values in the A100 format and is illustrative of the layout, not a capture from
a machine this project ran on. Everything TestForge itself emits about a GPU is
either measured or explicitly flagged as mock.

**Only NVIDIA.** No AMD (`rocm-smi`) or Intel support. The
`GpuDiagnosticProvider` interface is where that would go.

**Under WSL2, GPUs are typically not visible** even when the host has one,
unless passthrough is configured. TestForge distinguishes "no driver" from
"driver present, no visible device" in the reason text, because the fixes
differ.

---

## AI

**Output is probabilistic.** `SpecValidator` bounds what a generated test can
*do*; it cannot judge whether it is *good*. A generated test can be perfectly
valid and completely useless.

**Coverage scoring is a keyword heuristic.** `requirement_coverage` detects "no
test mentions the password rule at all". It cannot tell a correct test from a
plausible-looking wrong one. Only `execution_pass_rate`, which needs a live
service, reflects reality.

**Prompt-injection mitigations are mitigations, not controls.** A sufficiently
clever injection can change what the model *says*. What it cannot change is what
the system *does*, because the validator and the closed interpreter sit between
the two. The distinction is the design; see
[security.md](security.md#prompt-injection).

**No live model is exercised in CI.** Non-deterministic and costs money per run.
The contract, the validator and every failure path are tested against
`MockAiProvider`.

**One provider family.** The `AIProvider` interface is vendor-neutral, but only
an OpenAI-compatible client is implemented. `OPENAI_BASE_URL` points it at any
compatible endpoint, including a local model server.

**Cost is bounded, not free.** Caps on tests per request, requirement length,
context size and response size keep a runaway loop from becoming a large bill,
but live generation and analysis do cost money.

---

## Analytics

**Flake detection is heuristic**, and the same alternating pattern appears when
a real bug is intermittent, when a dependency was down for an hour, or when two
tests race over shared state. TestForge flags candidates; it does not diagnose.
The caveat is in the API payload, not only in the docs.

**Statistics are descriptive, not inferential.** No trend fitting, no anomaly
detection, no confidence intervals. Percentiles use nearest-rank, which is
always an observed value rather than an interpolated one, but coarse.

**Everything is single-database.** No cross-repository or cross-branch
aggregation.

---

## Persistence

**SQLite has one writer.** Two TestForge processes writing to the same database
will serialise on the file lock; with WAL and a busy timeout that is usually
invisible, but it is not a concurrent-write store.

**No automatic retention.** Pruning is a command (`testforge db prune`), not a
background task. A long-lived database will grow: roughly 1 KB per passing
result, 5–50 KB per failing one with diagnostics.

**No schema downgrade.** Migrations are forward-only. An older binary against a
newer database is not supported.

---

## Reporting

**The HTML report is a single static file.** No filtering, no sorting, no
search. It is meant to be opened, read and attached to a ticket; the dashboard is
where interaction lives.

**The dashboard is deliberately simple** and has no build step. It will not
scale to a complex UI, which is a trade recorded in
[design-decisions.md](design-decisions.md).

**Log capture is bounded** at 200 records per test and 500 per scope. A very
chatty test loses its earliest lines — the tail is what matters for triage.

**Report text is truncated** at documented limits: 400 characters for an
assertion haystack, 2 KB for a response excerpt, 16 KB for an error detail. Full
data is in the JSON report up to the storage caps.

---

## Benchmarks

**The numbers in the README are from one modest machine** — an Intel i3-4005U
with 4 logical cores, under WSL2. They are real measurements, and they are not
representative of a modern server.

Speed-up depends entirely on the workload. The benchmark reports I/O-bound and
CPU-bound separately for exactly that reason: a single headline number would be
true of one and misleading about the other.

**A benchmark on a busy machine measures the busy machine.** `scripts/benchmark.sh`
prints the load average before it starts, and the harness says so in its own
output.

---

## Testing

**Coverage is not measured.** No gcov or lcov integration. 395 tests cover every
module, but no percentage is claimed because an unmeasured one would be a guess.

**Property-based and fuzz testing are absent.** The JSON parser in particular
would benefit from a fuzzer; it currently has hand-written adversarial cases.

**No mutation testing**, so "the tests pass" is not the same as "the tests would
catch a regression" — except where a specific test exists for exactly that, as
in the failure-injection suite.

**The dashboard's JavaScript is untested.** ~200 lines of DOM rendering with no
logic worth asserting on; the API it consumes is tested.

**Two ThreadSanitizer suppressions are in force.** `.tsan-suppressions` silences
"double lock of a mutex" against `CancellationSource::cancel` and
`CancellationToken::waitFor`. The reasoning is written out in full in that file:
libstdc++ initialises `std::mutex` with `PTHREAD_MUTEX_INITIALIZER`, so TSan
never sees a `pthread_mutex_init` and infers the mutex's existence at its first
`lock()` — which, combined with the unlock/relock inside
`pthread_cond_timedwait`, makes it misattribute ownership. It is a real
suppression of a real report, and it is listed here rather than buried because
a suppression is a claim that should be checkable. Two things bound the risk:
it is scoped to two function names rather than a file or a whole category, and
**no data race is suppressed** — suppressing this noise is precisely what made
the genuine `HttpServer` race visible. Running with an empty suppressions file
reproduces the false positives and nothing else.

**TSan does not run out of the box on kernels with 32-bit ASLR entropy.** On
recent kernels it aborts with "unexpected memory mapping"; the workaround used
during development is `setarch $(uname -m) -R ./testforge_tests`. CI runs on
ubuntu-22.04, where it is not needed.

---

## Security

The full list is in [security.md](security.md#what-is-not-protected). The
headline items:

- no authentication or authorisation anywhere;
- no TLS;
- no sandboxing of test code — a compiled-in test runs with the process's full
  privileges, which is inherent to in-process execution and exactly why
  generated tests are specifications rather than code;
- redaction is best-effort pattern matching;
- no rate limiting;
- no audit log of who asked for what.

---

## Validation status

What has been executed locally, and what depends on an environment that was not
available.

**Validated locally.** Clean configure and build with `-Werror` (0 warnings),
the full GoogleTest suite, AddressSanitizer with leak detection,
UndefinedBehaviorSanitizer, ThreadSanitizer, `clang-format` at the version CI
uses, `pytest`, `ruff check` and `ruff format` at both the current release and
the version CI pins, the secret scan, the failure-injection negative check, the
offline AI evaluation, and CLI and REST smoke tests.

**Workflow definitions, not workflow runs.** The three GitHub Actions workflows
are committed and their YAML is valid, and every individual command inside them
has been run locally against this checkout. The workflows themselves have not
run on GitHub, so caching, matrix expansion, artifact upload and job summaries
are unexercised.

**Container images are configuration.** `Dockerfile`,
`sample-service/Dockerfile`, `python/Dockerfile` and `docker-compose.yml` are
written and parse, but no Docker daemon was available, so no image has been
built. The build they describe is the same sequence `scripts/build.sh` performs.

**clang-tidy runs at a newer version than CI installs.** The workflow uses
`clang-tidy-14`; only a much newer release was available locally, and its
findings are dominated by checks that postdate 14. It is an advisory job either
way. `clang-format` was run at 14.0.6, matching CI, and the tree passes the
exact CI command.

**GPU diagnostics need NVIDIA hardware.** The `nvidia-smi` CSV parser is a pure
function and is tested against captured real output, including a card that
reports `[N/A]` for sensors it lacks. The subprocess invocation itself requires
a machine with the driver installed. Mock output is labelled `is_mock_data:
true` at every level and the mock provider is not wired into any production
path. See [gpu-diagnostics.md](gpu-diagnostics.md).

**No OpenAI API key was ever used.** The AI paths were exercised entirely
through `MockAiProvider` and the offline evaluation harness. The live path is
implemented and its request construction is unit-tested, but no request has
ever been sent to OpenAI.

---

## Things that would come next

Where the seams are:

| | Why it is not here |
|---|---|
| Distributed execution | Large, and sharding covers the common case |
| A PostgreSQL repository | The interface exists; there is no use case yet |
| TLS in the C++ client | Would add OpenSSL to the dependency list for one feature |
| Coverage integration | Straightforward; not done |
| Fuzzing the JSON parser | The highest-value next test |
| AMD and Intel GPU providers | No hardware to verify against |
| An async job API for long runs | Needs a job store, polling and cancellation semantics; the CLI covers long runs today |
| Test dependencies and ordering constraints | Deliberately avoided — tests that depend on each other are a design smell |
