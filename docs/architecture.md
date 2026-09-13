# Architecture

How TestForge is put together, and why the pieces sit where they do.

- [Layers](#layers)
- [The dependency rule](#the-dependency-rule)
- [Module map](#module-map)
- [Test execution flow](#test-execution-flow)
- [Parallel execution](#parallel-execution)
- [The failure triage pipeline](#the-failure-triage-pipeline)
- [Diagnostics](#diagnostics)
- [Persistence](#persistence)
- [The AI layer](#the-ai-layer)
- [Interfaces](#interfaces)
- [Object model](#object-model)
- [CI/CD](#cicd)

---

## Layers

```mermaid
graph TB
    subgraph L4["Interfaces"]
        CLI["CLI<br/><i>src/cli</i>"]
        REST["REST server<br/><i>src/api</i>"]
        DASH["Dashboard<br/><i>dashboard/index.html</i>"]
    end

    subgraph L3["Service"]
        SVC["TestForgeService<br/><i>every operation, once</i>"]
    end

    subgraph L2["Engine"]
        REG["TestRegistry"]
        SEL["TestSelector"]
        RUN["TestRunner"]
        POOL["ThreadPool"]
        CLS["FailureClassifier"]
        ASR["AssertionEngine"]
    end

    subgraph L1["Support"]
        DIAG["diagnostics"]
        PERS["persistence"]
        REP["reporting"]
        NET["net"]
        AI["ai"]
    end

    subgraph L0["Core"]
        CORE["Json · Logger · Config · Process · Clock<br/>TestResult · TestCase · TestContext · CancellationToken · Interrupt"]
    end

    CLI --> SVC
    REST --> SVC
    DASH -.HTTP.-> REST
    SVC --> L2
    SVC --> L1
    L2 --> L1
    L2 --> L0
    L1 --> L0

    classDef l4 fill:#1f6feb22,stroke:#58a6ff
    classDef l2 fill:#3fb95022,stroke:#3fb950
    classDef l0 fill:#8b949e22,stroke:#8b949e
    class CLI,REST,DASH l4
    class REG,SEL,RUN,POOL,CLS,ASR l2
    class CORE l0
```

**Core** knows about nothing else. **Support** modules depend on core and on each
other only through interfaces. The **engine** depends on core and on the support
*interfaces*. The **service** wires concrete implementations together. The
**interfaces** layer is thin by design — `Commands.cpp` and `RestApiServer.cpp`
translate arguments and format output; neither contains logic the other lacks.

---

## The dependency rule

Every arrow below points *inwards*, towards the abstraction. Nothing in the
core knows the name of anything in the outer ring.

```mermaid
flowchart LR
    subgraph outer["adapters — replaceable"]
        SQL["SqliteResultRepository"]
        MEM["InMemoryRepository"]
        LIN["LinuxDiagnosticProvider"]
        NV["NvidiaGpuProvider"]
        MOCKG["MockGpuProvider"]
        HTTPAI["HttpAiProvider"]
        MOCKAI["MockAiProvider"]
        CONS["ConsoleReporter"]
        HTML["HtmlReporter"]
    end

    subgraph core["core — knows only interfaces"]
        RUNNER["TestRunner"]
        SVC["TestForgeService"]
    end

    SQL & MEM -->|implements| IREPO(["ResultRepository"])
    LIN -->|implements| IDIAG(["DiagnosticProvider"])
    NV & MOCKG -->|implements| IGPU(["GpuDiagnosticProvider"])
    HTTPAI & MOCKAI -->|implements| IAI(["AiProvider"])
    CONS & HTML -->|implements| IREP(["Reporter / RunObserver"])

    IREPO & IDIAG & IGPU & IAI & IREP --> core
```

The engine depends on abstractions, never on the things that implement them:

| The engine uses | It never mentions |
|---|---|
| `ResultRepository` | SQLite |
| `DiagnosticProvider` | `/proc`, `nvidia-smi` |
| `AIProvider` | OpenAI, HTTP |
| `Reporter` | HTML, JUnit XML |
| `RunObserver` | the console |

This is not abstraction for its own sake — each of those interfaces has more
than one implementation *in this repository*, and the second implementation is
usually what makes the first testable:

- `InMemoryRepository` lets `tests/integration/RunnerTest.cpp` exercise the
  entire runner — retries, fail-fast, persistence ordering — with no database,
  no schema and no file system.
- `MockGpuProvider` lets the GPU reporting path be tested on a machine with no
  GPU, which is every machine in this project's CI.
- `MockAiProvider` lets the whole generate → validate → execute pipeline run
  offline and deterministically.

---

## Module map

| Module | Responsibility | Key types |
|---|---|---|
| `core` | Values and services with no policy | `json::Value`, `Logger`, `Config`, `ProcessRunner`, `TestResult`, `TestContext`, `CancellationToken`, `interrupt` |
| `testing` | Authoring and selecting tests | `AssertionEngine`, `TestRegistry`, `TestSelector`, `ApiClient`, `SpecTestCase` |
| `execution` | Running them | `ThreadPool`, `TestRunner`, `FailureClassifier` |
| `net` | HTTP in both directions | `HttpClient`, `HttpServer`, `UrlPolicy` |
| `diagnostics` | What the machine looked like | `LinuxDiagnosticProvider`, `NvidiaGpuProvider`, `DiagnosticCollector` |
| `persistence` | Where results go | `ResultRepository`, `SqliteResultRepository`, `db::Connection` |
| `reporting` | What humans and CI read | `ConsoleReporter`, `JsonReporter`, `HtmlReporter`, `JUnitReporter` |
| `ai` | Model integration, safely | `AIProvider`, `TestSpec`, `SpecValidator`, `FailureContext` |
| `api` | One entry point for both front ends | `TestForgeService`, `RestApiServer` |

---

## Test execution flow

```mermaid
sequenceDiagram
    participant U as CLI / REST
    participant S as TestForgeService
    participant Sel as TestSelector
    participant R as TestRunner
    participant W as worker thread
    participant T as TestCase
    participant C as FailureClassifier
    participant D as DiagnosticCollector
    participant P as ResultRepository
    participant Rep as Reporters

    U->>S: run(filter, workers, retries)
    S->>Sel: select(registry, filter)
    Sel-->>S: ordered TestMetadata[]
    S->>R: run(options)
    R->>P: beginRun()

    loop each selected test
        R->>W: submit
        W->>T: construct (fresh instance)
        W->>T: setUp(ctx)
        W->>T: execute(ctx)
        alt threw
            W->>C: classify(exception)
            C-->>W: status + category
            W->>C: refine(category, recorded metadata)
            W->>D: collectForFailure()
        end
        W->>T: tearDown(ctx) — noexcept
        W->>P: saveResult()
        W-->>R: TestResult
        R-->>U: onTestFinished (live line)
    end

    R->>P: completeRun()
    S->>Rep: render + write
    S-->>U: TestRun + exit code
```

Three properties hold regardless of what the model returns:

- **Results are persisted as each test finishes**, not batched at the end. An
  interrupted run keeps what it already produced. `InMemoryRepository` counts the
  calls so a test can assert this.
- **`tearDown` is `noexcept`.** A throwing teardown would mask the original
  failure — the thing the engineer actually needs.
- **Tests are constructed per execution.** The registry holds factories, so a
  retry cannot inherit state from the attempt that failed and two workers can
  never share an instance.

---

## Parallel execution

```mermaid
flowchart TB
    SEL["selected tests<br/><i>sorted, sharded, optionally shuffled</i>"] --> Q(["thread-safe queue<br/>mutex + condition_variable"])
    Q --> W0[worker-0] & W1[worker-1] & W2[worker-2] & WN[worker-N]

    W0 & W1 & W2 & WN --> RES(["results vector<br/><i>mutex-guarded</i>"])
    RES --> SORT["sorted by qualified name<br/><i>so reports are comparable</i>"]

    W1 -.->|"timeout"| ASYNC["helper thread<br/><i>thread + promise</i>"]
    ASYNC -.->|"abandoned"| REAP["OrphanReaper<br/><i>bounded grace at shutdown</i>"]
```

Completion order is nondeterministic; **report order is not**. Results are sorted
by qualified name before rendering, so two runs of the same suite produce
comparable output.

The full concurrency model, including what happens to an uncooperative test, is
in [concurrency.md](concurrency.md).

---

## The failure triage pipeline

```mermaid
flowchart TD
    EXEC["test executes"] --> OUT{outcome}
    OUT -->|"returns"| PASS["PASSED"]
    OUT -->|"SkipTest"| SKIP["SKIPPED"]
    OUT -->|"throws"| P1["pass 1 — classify from exception type"]

    P1 --> P1A["AssertionFailure → FAILED / ASSERTION_FAILURE"]
    P1 --> P1B["TestForgeError → ERROR / its own category"]
    P1 --> P1C["TestCancelled → TIMEOUT"]
    P1 --> P1D["std::exception → ERROR, category from the message"]
    P1 --> P1E["anything else → ERROR / UNKNOWN"]

    P1A & P1B & P1C & P1D & P1E --> P2["pass 2 — refine from recorded evidence"]
    P2 --> P2A["http_status ≥ 500 → APPLICATION_FAILURE"]
    P2 --> P2B["transport_error → NETWORK_FAILURE"]
    P2 --> P2C["missing_dependency → DEPENDENCY_FAILURE"]

    P2A & P2B & P2C --> DIAG["collect diagnostics"]
    DIAG --> STORE["persist"]
    STORE --> REPORT["report"]
    REPORT --> AI{"AI enabled?"}
    AI -->|yes| ADV["advisory analysis<br/><i>does not alter the verdict</i>"]
    AI -->|no| DONE["done"]
    ADV --> DONE
    PASS --> DONE
    SKIP --> DONE
```

Refinement never makes a diagnosis vaguer. `FailureClassifier` ranks categories
by specificity and only promotes, so a `TIMEOUT` is not downgraded to a generic
application failure because the response happened to be a 500.

---

## Diagnostics

```mermaid
flowchart LR
    C["DiagnosticCollector"] --> S["LinuxDiagnosticProvider"]
    C --> G["GpuDiagnosticProvider<br/><i>interface</i>"]

    S --> P1["/proc/cpuinfo, /proc/meminfo,<br/>/proc/stat, /proc/mounts"]
    S --> P2["/sys/class/net/*/statistics"]
    S --> P3["uname, statvfs, getifaddrs"]

    G --> N["NvidiaGpuProvider<br/><i>nvidia-smi via ProcessRunner</i>"]
    G --> M["MockGpuProvider<br/><i>tests only, always labelled</i>"]
    G --> NL["NullGpuProvider<br/><i>no hardware: says so</i>"]
```

`createDefaultGpuProvider()` returns `NvidiaGpuProvider` when `nvidia-smi` is on
`PATH` and `NullGpuProvider` otherwise. It never returns the mock.

---

## Persistence

```mermaid
erDiagram
    test_runs ||--o{ test_results : contains
    test_runs ||--o{ test_events : records

    test_runs {
        TEXT run_id PK
        TEXT label
        INTEGER started_at_ms
        INTEGER finished_at_ms
        INTEGER workers
        TEXT filter
        TEXT git_commit
        TEXT environment_json
        INTEGER passed
        INTEGER failed
    }
    test_results {
        INTEGER id PK
        TEXT run_id FK
        TEXT test_id "stable hash of suite+name"
        TEXT qualified_name
        TEXT status
        TEXT failure_category
        INTEGER duration_ms
        TEXT error_detail
        TEXT metadata_json
        TEXT diagnostics_json
        INTEGER attempt
    }
    test_events {
        INTEGER id PK
        TEXT run_id FK
        TEXT event_type
        TEXT payload_json
    }
```

`test_id` is a deterministic hash of suite + name, so the same test carries the
same identity across runs and across machines — which is what makes history and
flake analysis possible at all. Details in [database.md](database.md).

---

## The Python sidecar

The only place C++ and Python meet is one HTTP hop on loopback. The boundary
exists because the C++ client links no TLS stack and so *cannot* reach
`api.openai.com` even if someone wanted it to.

```mermaid
sequenceDiagram
    autonumber
    participant CLI as testforge ai generate-tests
    participant SVC as TestForgeService
    participant PROV as HttpAiProvider
    participant SIDE as Python sidecar (uvicorn :8810)
    participant OAI as OpenAI

    CLI->>SVC: generateTests(requirement, suite)
    SVC->>PROV: generate(request)
    Note over PROV: UrlPolicy::restrictedTo(endpoint)<br/>followRedirects = false
    PROV->>SIDE: POST /generate-tests (plain HTTP, loopback)

    alt key present and SDK installed
        SIDE->>OAI: chat.completions<br/>response_format=json_object
        OAI-->>SIDE: JSON object
    else no key, or no openai package
        SIDE-->>PROV: 200 {ok:false, reason:"..."}<br/>never a fabricated answer
    end

    SIDE-->>PROV: {tests:[...], usage:{...}}
    PROV-->>SVC: GenerationResult
    SVC->>SVC: SpecValidator::validate
    SVC->>SVC: registerSpecSuite — validates a second time
    SVC-->>CLI: accepted / rejected, with reasons
```

Three properties hold regardless of what the model returns:

1. **The sidecar never invents an answer.** No key, no SDK, or a non-JSON reply
   all produce an explicit failure, because a fabricated analysis is worse than
   none.
2. **The engine re-validates.** `registerSpecSuite` does not trust its caller —
   the same allow-list runs again at registration.
3. **A specification is data.** It becomes a `SpecTestCase` interpreting a
   closed enum, so there is nothing for a hostile specification to execute.

---

## The AI layer

```mermaid
flowchart TB
    subgraph CPP["C++ process"]
        SVC["TestForgeService"] --> PROV["AIProvider<br/><i>interface</i>"]
        PROV --> HTTP["HttpAiProvider"]
        PROV --> MOCK["MockAiProvider<br/><i>offline, deterministic</i>"]
        PROV --> DIS["DisabledAiProvider<br/><i>explains why</i>"]
        VAL["SpecValidator<br/><b>the security boundary</b>"]
        EXEC["SpecTestCase<br/><i>closed interpreter</i>"]
    end

    subgraph PY["Python sidecar"]
        API["/generate-tests<br/>/analyze-failure"] --> PR["prompts.py"]
        API --> SCH["schema.py"]
        API --> KEY(["OPENAI_API_KEY<br/><i>lives only here</i>"])
    end

    HTTP -->|"HTTP, loopback"| API
    API -->|"HTTPS"| MODEL["model provider"]
    HTTP --> VAL
    MOCK --> VAL
    VAL -->|accepted| EXEC
    VAL -->|rejected| DROP["dropped, with a reason"]
```

The boundary is `SpecValidator`, not the prompt. Prompt instructions are a
mitigation; the control is that a generated test can only express things
`SpecTestCase` already knows how to do. See [ai-architecture.md](ai-architecture.md)
and [security.md](security.md).

---

## Persistence flow

Where a result goes, and when. Persistence never blocks a verdict: a database
error is logged and the run continues, because the results are the product and
the database is a convenience.

```mermaid
sequenceDiagram
    autonumber
    participant R as TestRunner
    participant W as worker
    participant REPO as ResultRepository
    participant DB as SQLite (WAL)
    participant OUT as ReportWriter

    R->>REPO: beginRun(run)
    REPO->>DB: INSERT INTO test_runs
    Note over REPO,DB: a failure here is logged,<br/>not propagated

    loop each test
        W->>W: execute, classify
        W->>REPO: saveResult(result)
        REPO->>DB: INSERT INTO test_results (bound parameters)
    end

    R->>REPO: finishRun(run)
    REPO->>DB: UPDATE test_runs SET counts, finished_at
    R->>OUT: write(run)
    OUT->>OUT: console · JSON · HTML

    Note over DB: readers (dashboard, `history`)<br/>are never blocked: WAL
```

Reads take the same path in reverse: `historySummaries()` pulls the last 50
outcomes per test to compute flip rate and p95, and `listRuns()` reads stored
counts rather than re-loading every result row — a listing of 50 runs must not
load 50,000 results.

---

## Interfaces

| Interface | Implementations | What the second one buys |
|---|---|---|
| `TestCase` | `FunctionTestCase`, `ApiTestCase`, `SpecTestCase` | macro-authored, class-authored and generated tests share one runner |
| `ResultRepository` | `SqliteResultRepository`, `InMemoryRepository` | the runner is testable with no database |
| `DiagnosticProvider` | `LinuxDiagnosticProvider`, GPU providers | new sources without touching the runner |
| `GpuDiagnosticProvider` | `NvidiaGpuProvider`, `MockGpuProvider`, `NullGpuProvider` | GPU code paths testable with no GPU |
| `AIProvider` | `HttpAiProvider`, `MockAiProvider`, `DisabledAiProvider` | offline demos, CI, and graceful degradation |
| `Reporter` | console, JSON, HTML, JUnit | one run, four audiences |
| `RunObserver` | `ConsoleReporter` | live output without the runner knowing about a terminal |
| `LogSink` | console, file, memory | tests can capture and assert on logs |

---

## Object model

```mermaid
classDiagram
    class TestCase {
        <<interface>>
        +setUp(TestContext&)
        +execute(TestContext&)*
        +tearDown(TestContext&) noexcept
        +metadata() TestMetadata*
    }
    class FunctionTestCase
    class ApiTestCase
    class SpecTestCase

    class TestContext {
        +config() Config
        +log() Logger
        +cancellation() CancellationToken
        +throwIfCancelled()
        +addMetadata(key, value)
        +skip(reason)
    }

    class TestResult {
        +testId, runId
        +status, failureCategory
        +duration, logs
        +metadata, diagnostics
        +toJson()
    }

    class TestRunner {
        +run(RunOptions) TestRun
        +runOne(...) TestResult
        +setRepository(...)
        +addObserver(...)
    }

    TestCase <|-- FunctionTestCase
    TestCase <|-- ApiTestCase
    TestCase <|-- SpecTestCase
    TestRunner ..> TestCase : constructs per execution
    TestRunner ..> TestContext : provides
    TestRunner --> TestResult : produces
```

---

## CI/CD

```mermaid
flowchart LR
    PUSH([push / PR]) --> B["build.yml<br/>gcc × clang<br/>Debug × Release<br/>-Werror"]
    PUSH --> T["tests.yml"]
    PUSH --> Q["quality.yml<br/>format · tidy · secrets"]

    T --> T1["unit + failure injection"]
    T --> T2["integration vs live service"]
    T --> T3["ASan/UBSan · TSan"]
    T --> T4["python + AI evaluation"]

    T1 --> NEG["assert the injected suite<br/><b>actually fails</b>"]
    T2 --> SUM["job summary + artifacts"]
    B --> DOCKER["docker build<br/><i>runs its own tests</i>"]
```

The `NEG` step is the unusual one: it asserts the *negative*. The
failure-injection suite must exit `1` and must produce the failure categories it
advertises. A test tool that stops detecting failures would otherwise pass CI
silently.
