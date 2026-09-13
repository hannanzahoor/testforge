# Security

The threat model, the controls, and what is deliberately *not* protected.

TestForge is a developer and CI tool. It is not multi-tenant, it has no
authentication, and it is not built to survive exposure to the public internet.
Within that scope, the controls below are real and tested.

- [Threat model](#threat-model)
- [Command execution](#command-execution)
- [SQL](#sql)
- [AI output](#ai-output)
- [Prompt injection](#prompt-injection)
- [Network](#network)
- [Secrets](#secrets)
- [Files](#files)
- [The REST API](#the-rest-api)
- [Untrusted input inventory](#untrusted-input-inventory)
- [What is not protected](#what-is-not-protected)
- [Verification](#verification)

---

## Threat model

**Assumed trusted:** the person running TestForge, the C++ test code compiled
into the binary, and the configuration file on disk.

**Assumed untrusted:**

| Source | Why |
|---|---|
| Language-model output | An external service, shaped by a prompt that may itself contain text from the system under test. |
| HTTP responses from the SUT | The thing being tested is, by definition, possibly broken or malicious. |
| Test specification files | Hand-written or generated; either way, input. |
| Requirement text | Frequently pasted from a ticket, an email or a web page. |
| Environment variables | Set by CI configuration nobody re-reads. |
| Anything read from `/proc` | Contents are influenced by other processes on the host. |
| Data loaded back from the database | It came from one of the above. |

**Primary risks:** command injection through diagnostics, SQL injection through
result storage, arbitrary requests through generated tests, credential leakage
into logs and reports, and path traversal through static file serving.

---

## Command execution

**Control: there is no shell.**

`ProcessRunner` (`src/core/Process.cpp`) is the only place in the codebase that
starts a process. It calls `execv` with an argv array. `system()` and `popen()`
appear nowhere.

```cpp
// The executable is resolved through an explicit PATH search; a name
// containing '/' is treated as a path and never searched for.
const std::string resolved = which(executable);

// argv is built before forking; between fork and exec only
// async-signal-safe calls are made.
::execv(program.c_str(), argv.data());
```

Because there is no shell, shell metacharacters in an argument are inert data.
A semicolon is a character in a filename, not a command separator.

Additional constraints on every invocation:

- arguments containing NUL or newline are rejected before `fork`;
- a timeout, after which the child's **process group** is sent `SIGTERM` and
  then `SIGKILL` — the group, because `nvidia-smi` may have spawned children;
- an output cap, so a runaway child cannot exhaust memory;
- `stdin` redirected to `/dev/null`, so a diagnostic command can never block
  waiting for input or consume the parent's stdin.

**Diagnostics avoid subprocesses almost entirely.** CPU, memory, disk, network
and process information are read from `/proc`, `/sys`, `statvfs` and
`getifaddrs`. The only external commands are `nvidia-smi` (fixed argument
vector, nothing user-supplied), and `journalctl`/`dmesg` for system logs — both
off by default because they usually need privileges.

**Verified by** `Process.ShellMetacharactersAreInertData`, which passes
``"a; touch /tmp/testforge-pwned; $(whoami) `id` && echo no"`` as an argument
and asserts it comes back verbatim, and by
`system.subprocess_execution_is_safe` in the bundled suite.

---

## SQL

**Control: parameterised queries, always.**

The `db::Statement` wrapper has no interface for splicing text into SQL. Values
are bound:

```cpp
db::Statement insert(connection,
    "INSERT INTO test_results (run_id, test_name, error_message) VALUES (?, ?, ?);");
insert.bindAll(runId, testName, errorMessage);
insert.execute();
```

Where a clause must be assembled — `listRuns` builds its `WHERE` from optional
filters — the *fragments* are fixed literals and every value is still bound.

The one identifier that cannot be a parameter is a PRAGMA name, so
`Connection::pragma` validates the name against `[A-Za-z0-9_]` and throws
otherwise. Every call site passes a literal; the check is there so that remains
true after someone edits the file.

SQLite is also compiled with `SQLITE_DQS=0` (no double-quoted string literals,
which removes a class of accidental-identifier confusion) and
`SQLITE_OMIT_LOAD_EXTENSION` (no run-time extension loading).

**Verified by** `Database.ParametersAreBoundNotInterpolated`, which inserts
`Robert'); DROP TABLE t;--` and asserts both that it round-trips as data and
that the table still exists.

---

## AI output

**Control: a generated test is data with a closed vocabulary, not code.**

A `TestSpec` can express a method, a relative path, headers, a body, an expected
status, a latency budget, and assertions from a closed list of nine kinds.
`SpecTestCase` is a plain interpreter over exactly those fields — no `eval`, no
code generation, no shell, no dynamic loading. A malicious specification cannot
do anything a well-behaved one could not, because every field it can set is one
the interpreter already knows how to handle.

`SpecValidator` (`src/ai/SpecValidator.cpp`) then enforces:

| Rule | Rationale |
|---|---|
| Endpoint must start with `/` | A generated test may not choose its own destination host |
| No scheme, no `@`, no `//` prefix | Blocks absolute and protocol-relative URLs |
| No `..` | Traversal is refused, not normalised |
| No control characters | A CR or LF would be HTTP request splitting |
| Method from an allow-list | No `TRACE` (cross-site tracing), no `CONNECT` (tunnelling) |
| Header deny-list | `Authorization`, `Cookie`, `Host`, `X-Forwarded-*`, `Content-Length`, `Transfer-Encoding` — credentials and routing come from configuration |
| Status in 100–599 | |
| Names restricted to `[A-Za-z0-9_.-]` | A name becomes part of an identifier, a filename and an HTML fragment |
| Unique names within a suite | A duplicate would silently replace the first |
| Body ≤ 64 KiB, ≤ 16 headers, ≤ 20 assertions | Bounds |
| The resolved URL must pass `UrlPolicy` | Catches a base URL the operator has since restricted |

A test that violates any fatal rule is **dropped with a recorded reason**, never
repaired. Repairing would mean running something nobody reviewed.

**Validation runs three times:** in the Python sidecar (good messages), in
`SpecValidator` (the boundary), and again inside `registerSpecSuite` immediately
before registration — so a caller that skipped the second cannot get anything
runnable.

```
$ testforge spec load evil.json
  accepted              1
  rejected              5
  rejected  tests[0].endpoint: endpoint must be a path beginning with '/'
  rejected  tests[1].endpoint: endpoint must not contain '..'
  rejected  tests[2].headers: header 'Authorization' may not be set by a generated test
  rejected  tests[3].name: name contains an unsupported character ' '
  rejected  tests[4].method: method 'CONNECT' is not allowed
```

---

## Why arbitrary code execution is impossible here

The usual worry about "AI writes tests" is that generated text becomes
generated code. In TestForge it cannot, and the reason is structural rather
than a matter of filtering harder.

A validated specification becomes a `SpecTestCase`, which is an **interpreter
over a closed enum**:

```cpp
enum class SpecAssertion::Kind {
    StatusCode, StatusCodeIn, ResponseTimeUnder,
    BodyContains, BodyNotContains,
    JsonFieldExists, JsonFieldEquals,
    HeaderExists, HeaderEquals,
};
```

Nine members. `SpecTestCase::execute` is a `switch` over them. There is no
code generation step, no compiler invoked, no `eval`, no `dlopen`, no plugin
path, no template rendered into a source file. A specification that asks for
something outside those nine cannot be represented, let alone executed:

```
$ grep -rnE "dlopen|system\(|popen\(|execv|execl|eval\(" src/ai/ src/testing/SpecTestCase.cpp
(no matches)
```

The only side effect a generated test can have is **one HTTP request to a
relative path on the configured base URL**, and that path has already been
through the allow-list and the URL policy twice.

This is what bounds the blast radius of a successful prompt injection. Even a
model that fully complies with an attacker can only emit JSON describing an
HTTP assertion — and the validator then rejects the parts it is not allowed to
describe.

---

## Prompt injection

A requirement can be pasted from a ticket. A failure context contains response
bodies from the system under test. Either can contain text shaped like an
instruction.

**Mitigations** (in `python/ai/prompts.py` and `python/ai/service.py`):

- payloads are sent as **JSON inside a user message**, never concatenated into
  the system prompt;
- both system prompts state explicitly that the payload is data, that it may
  contain text directed at the model, and that such text must be treated as a
  description of the system under test;
- the failure context is labelled `trust_level: "untrusted_data"` in the
  document itself;
- the response must be a JSON object (`response_format={"type":"json_object"}`),
  so prose cannot be smuggled through the schema.

**These are mitigations, not controls.** A sufficiently clever injection can
change what the model *says*. What it cannot change is what the system *does*,
because:

- a generated specification still has to pass `SpecValidator`;
- an analysis cannot alter a verdict — the verdict was decided before the call;
- the model has no tools, no function calling and no side effects.

The security property comes from the boundary, not the prompt. That distinction
is the design.

---

## Network

**Control: `UrlPolicy`, applied before a socket is opened.**

```cpp
UrlPolicy policy;
policy.blockLinkLocal = true;   // default
policy.reject("http://169.254.169.254/latest/meta-data/");
// -> "link-local addresses (including cloud metadata endpoints) are blocked"
```

| Rule | Default | Why |
|---|---|---|
| Scheme allow-list (`http`, `https`) | on | No `file:`, `ftp:`, `gopher:` |
| Block link-local (`169.254.0.0/16`, `fe80::`) | on | `169.254.169.254` is the cloud metadata endpoint on AWS, GCP and Azure |
| Block loopback | off | The sample service runs on localhost; that is the point |
| Block RFC 1918 | off | Same reason |
| Host allow-list | set per client | `UrlPolicy::restrictedTo(baseUrl)` pins the API client and the AI client to one host each |

Also enforced: response size caps (headers 64 KiB, body configurable), request
timeouts with cancellation, redirect limits, and `MSG_NOSIGNAL` on every send so
a closed peer cannot kill the process with `SIGPIPE`.

**Known limit:** the policy checks the literal host in the URL and does not
re-resolve after connecting, so it does not defend against DNS rebinding. For a
tool pointed at a service you control, that is an acceptable gap; it is recorded
here rather than glossed over.

---

## Secrets

**Control: one choke point, plus an allow-list for the environment.**

The OpenAI key exists only in the Python sidecar process. The C++ engine talks
to the sidecar over plain HTTP on loopback and never handles a credential.

Everything that reaches a log sink, a report field or an AI payload passes
through `strings::redactSecrets`, which recognises:

- `Authorization: Bearer <token>` and bare `Bearer <token>`;
- provider prefixes: `sk-`, `ghp_`, `gho_`, `xoxb-`, `AKIA…`;
- `key: value` / `key=value` where the key name contains `password`, `secret`,
  `token`, `api_key`, `apikey` or `authorization`.

The function is **idempotent** — this matters, and was a real bug: an earlier
version recursed after each substitution and looped forever the second time it
met its own mask, taking the process down with a stack overflow.
`Strings.RedactionIsIdempotent` exists to keep it fixed.

Environment capture uses a **curated allow-list** (`kDiagnosticNames`: `PATH`,
`HOME`, `CI`, `LANG`, `CUDA_VISIBLE_DEVICES`, …). Anything not on the list is
omitted rather than redacted, so a variable nobody anticipated cannot leak.
`env::snapshotRedacted()` additionally masks any variable whose *name* looks
sensitive.

`FailureContext` applies the same reasoning to result metadata: an allow-list of
twelve keys, so a new field added by a test does not reach the model by default.

CI fails the build on committed credential shapes
(`.github/workflows/quality.yml`), and `.env` is in `.gitignore` with a check
that it is not tracked.

**Known limit:** redaction recognises common shapes and nothing else. A
credential in an unusual format will pass through. It is defence in depth, not a
guarantee.

---

## Files

| Surface | Control |
|---|---|
| Static file serving | `isSafeRelativePath` rejects absolute paths, `..`, `.`, backslashes and NUL. Refused, not normalised. |
| Report output | Paths are built from the configured directory plus a run id and a timestamp; no user input reaches a filename. Colons are stripped so the names are valid on every filesystem. |
| `.env` loading | Parses `KEY=VALUE`; never overwrites an existing variable, so the real environment always wins. |
| Config loading | JSON only, with depth and length limits. |
| Log files | Opened `"ae"` — append and close-on-exec, so a forked diagnostic command does not inherit the descriptor. |

**Why refuse rather than normalise.** Resolving `../..` silently turns a typo in
a generated specification into a request nobody intended. Refusing is easier to
reason about and impossible to get subtly wrong.

---

## The REST API

**No authentication.** `POST /api/runs` executes registered tests. Anyone who can
reach the port can run them.

Mitigations:

- binds `127.0.0.1` by default, in code and in `config/default.json`;
- `docker-compose.yml` publishes it as `127.0.0.1:8080:8080`;
- request size limits (1 MiB by default), a 10-second socket timeout, and a
  bounded worker pool;
- a handler that throws becomes a 500, never a crashed server;
- `X-Content-Type-Options: nosniff` on every response;
- the constraint is stated in the header of `RestApiServer`, in `testforge
  serve --help`, and here.

**If you need to expose it,** put it behind a reverse proxy that terminates TLS
and authenticates. TestForge does neither.

---

## Untrusted input inventory

| Input | Reaches | Control |
|---|---|---|
| Model output | `SpecTestCase` | `SpecValidator` allow-lists |
| Requirement text | The prompt | JSON in a user message; size cap |
| HTTP response bodies | Reports, AI context | Escaped in HTML/XML, truncated, redacted |
| Test names from a spec | Registry, filenames, HTML | Character allow-list |
| Config files | Everything | JSON limits, `validate()` |
| Environment variables | Config, diagnostics | Allow-list, type-checked parsing |
| `/proc` contents | Diagnostics | Parsed defensively; failures become "unavailable" |
| Database rows | Reports, API | JSON re-parsed with limits; unknown fields ignored |
| CLI arguments | Everything | Unknown options rejected; values type-checked |
| HTTP requests to the API | The service | Size limits, JSON limits, bounded parameters |

---

## Credentials and redirects

`HttpClient` follows redirects by copying the original request, headers
included. If the target is a different origin, that copy would hand its
operator whatever `Authorization` or `Cookie` the caller attached for the
*first* host — the leak curl gates behind `--location-trusted`.

TestForge refuses it outright. On any change of scheme, host or effective port,
`Authorization`, `Cookie` and `Proxy-Authorization` are dropped before the
follow-up request is built. Ordinary headers are kept, so tracing and content
negotiation still work. A credential is not restored if a later hop returns to
the original host: once it has left, it is gone for the rest of the chain,
which stops a hostile intermediary bouncing a request home to read the header.

The URL policy is also re-applied on every hop, because redirects recurse
through the same `perform()` that checks it. Both defences are needed: the
default policy allows any host, so the policy alone would not stop the leak.

`tests/integration/HttpTest.cpp` covers same-origin, cross-origin, both
credential kinds, a chain that crosses the boundary mid-way, and a chain that
comes back.

---

## Bounding what the server accepts

The acceptor hands each connection to a worker pool whose queue has no depth
limit, so every accepted-but-unhandled connection is a descriptor the process
holds for up to the socket read timeout. A client opening sockets faster than
the workers drain them could exhaust the descriptor table.

`ServerConfig::maxPendingConnections` (default 256) caps connections in flight.
Past it, a connection is answered `503 SERVER_BUSY` and closed immediately —
bounded and visible, rather than a slow slide into descriptor exhaustion.
`HttpServer::connectionsRejected()` exposes the count so the behaviour is
testable rather than only inferable.

Chunked request bodies are rejected with `400 UNSUPPORTED_TRANSFER_ENCODING`.
Only `Content-Length` is honoured when reading a body, so such a request used
to reach the handler with an empty body and no sign anything was wrong.

---

## What is not protected

The controls above have limits. These are the ones that matter.

- **No authentication or authorisation.** Anyone who can reach the API can use
  all of it.
- **No TLS.** The C++ client and server both speak plain HTTP.
- **No sandboxing of test code.** A test compiled into the binary runs with the
  process's full privileges. This is inherent to in-process test execution and
  is exactly why generated tests are *specifications* rather than code.
- **No DNS rebinding defence**, as noted above.
- **No rate limiting** on the API or on AI calls beyond per-request size and
  count caps.
- **Redaction is best-effort.**
- **No supply-chain verification beyond a pinned hash.** SQLite is verified by
  SHA-256; GoogleTest is pinned by tag, not by hash.
- **No audit log.** `test_events` records what the tool did, not who asked.

---

## Verification

| Control | Test |
|---|---|
| No shell execution | `Process.ShellMetacharactersAreInertData`, `system.subprocess_execution_is_safe` |
| Argument sanitisation | `Process.ArgumentsWithNulOrNewlineAreRejected` |
| Subprocess timeout | `Process.TimeoutTerminatesTheChild` |
| SQL parameterisation | `Database.ParametersAreBoundNotInterpolated` |
| Absolute URLs rejected | `SpecValidator.RejectsAbsoluteUrls` |
| Traversal rejected | `SpecValidator.RejectsPathTraversal`, `UrlJoin.RejectsTraversalRatherThanNormalisingIt` |
| Request splitting rejected | `SpecValidator.RejectsRequestSplitting` |
| Credential headers rejected | `SpecValidator.RejectsCredentialAndRoutingHeaders` |
| Method allow-list | `SpecValidator.RejectsDisallowedMethods` |
| Metadata allow-list | `FailureContext.MetadataUsesAnAllowList` |
| Cloud metadata blocked | `UrlPolicy.BlocksCloudMetadataByDefault` |
| Host pinning | `UrlPolicy.AllowListPinsToOneHost`, `HttpFixture.UrlPolicyBlocksBeforeASocketIsOpened` |
| Redaction | `Strings.RedactsBearerTokens`, `…ProviderKeyShapes`, `…KeyValuePairs` |
| Redaction is idempotent | `Strings.RedactionIsIdempotent` |
| Environment redaction | `Environment.DiagnosticSubsetRedactsSensitiveNames`, `system.environment_snapshot_redacts_secrets` |
| AI context redaction | `FailureContext.RedactsWhatItDoesInclude` |
| HTML escaping | `HtmlReporter.EscapesContentFromTheSystemUnderTest` |
| XML escaping | `JUnitReporter.EscapesXmlMetacharacters` |
| Request size limits | `HttpFixture.OversizedRequestsAreRejected` |
| Response size caps | `HttpFixture.ResponseSizeIsCapped` |
| JSON depth limit | `JsonParse.DepthLimitIsEnforced` |
| Handler exceptions contained | `HttpFixture.AThrowingHandlerBecomesA500NotACrash` |
| No committed secrets | `.github/workflows/quality.yml` |

The whole suite also runs under AddressSanitizer, UndefinedBehaviorSanitizer and
ThreadSanitizer in CI.

**Reporting a problem.** See [SECURITY.md](../SECURITY.md).
