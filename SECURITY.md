# Security Policy

## Reporting a vulnerability

Open a GitHub security advisory on this repository, or a regular issue if the
finding is not sensitive. Please include the version or commit, the platform,
and a reproduction.

## Intended deployment

TestForge is a developer and CI tool. It binds to loopback by default and has
no authentication layer. It is designed to run on a build agent or a developer
workstation, against services the operator controls.

The REST API executes registered tests. Exposing it to an untrusted network
would let anyone who can reach the port run them, which is why `server.host`
defaults to `127.0.0.1` and `docker-compose.yml` publishes every port as
`127.0.0.1:<port>`.

## Security boundaries

These are enforced in code and covered by tests.

**No shell.** `ProcessRunner` is the only place the project starts a process. It
uses `fork` + `execv`/`execve` with an argument vector; `system()` and `popen()`
appear nowhere. Shell metacharacters in an argument are ordinary bytes, and
`Process.ArgumentsWithNulOrNewlineAreRejected` asserts exactly that. Children
get their own process group so a timeout can terminate descendants, stdin from
`/dev/null`, an output cap, and a deadline.

**Credentials do not cross an origin.** When `HttpClient` follows a redirect to
a different scheme, host, or port, it removes `Authorization`, `Cookie`, and
`Proxy-Authorization` before issuing the follow-up request, and does not restore
them if a later hop returns to the original host. The URL policy is re-applied
on every hop.

**Generated test specifications are validated twice.** Anything a model produces
passes `SpecValidator` before registration and again inside
`registerSpecSuite`, which does not trust its caller. The validator allow-lists
HTTP methods and assertion kinds, requires relative endpoints (no scheme, no
protocol-relative `//`, no `..`, no `@`, no control characters), denies routing
and credential headers, resolves the endpoint against the configured base URL,
and re-checks it under the URL policy.

**Generated tests cannot execute code.** A validated specification becomes a
`SpecTestCase`, an interpreter over a closed nine-member assertion enum. There
is no code generation, compilation, `eval`, or `dlopen` anywhere in that path.
The only side effect available to a generated test is one HTTP request to a
validated relative path.

**SQL values are always bound.** The only string concatenation in the query
layer is a compile-time column list and fixed `WHERE` fragments selected by
boolean flags.

**Server-side request forgery.** The URL policy blocks link-local addresses,
including the cloud metadata endpoint, by default. It matches on the literal
host in the URL and does not re-resolve after connecting, so it is a policy
control rather than a defence against DNS rebinding.

**Secrets.** Credential-shaped values are redacted from logs, reports, and the
diagnostic environment snapshot. A blocking CI job scans the tree for
credential shapes; test fixtures that must contain such strings are marked
`NOT-A-SECRET` per occurrence, and the job prints how many exemptions exist so
an unexplained increase is visible in review. API keys are read from the
environment and never appear in configuration structures.

## Known constraints

- No TLS. The HTTP client refuses `https://` with an explicit message rather
  than failing obscurely, and the AI sidecar is reached over loopback HTTP.
- No authentication on the REST API.
- Static file serving does not resolve symlinks; the served directory is
  assumed to be operator-controlled.
- ThreadSanitizer runs with two suppressions, scoped to two function names, for
  a libstdc++ mutex-lifecycle false positive documented in
  `.tsan-suppressions`. No data race is suppressed.

Details and the full threat model are in [docs/security.md](docs/security.md).
