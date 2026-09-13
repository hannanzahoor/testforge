#include "testforge/api/TestForgeService.hpp"

#include "testforge/ai/FailureContext.hpp"
#include "testforge/ai/SpecValidator.hpp"
#include "testforge/core/Exceptions.hpp"
#include "testforge/core/Ids.hpp"
#include "testforge/core/Logger.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/core/Version.hpp"
#include "testforge/net/Url.hpp"
#include "testforge/persistence/SqliteResultRepository.hpp"
#include "testforge/reporting/Reporter.hpp"
#include "testforge/testing/SpecTestCase.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace testforge::api {
namespace {

Logger& logger() {
    static Logger instance("service");
    return instance;
}

json::Value errorObject(const std::string& message, const std::string& code = "ERROR") {
    json::Value out = json::Value::object();
    out.set("ok", false);
    out.set("error", message);
    out.set("code", code);
    return out;
}

}  // namespace

TestForgeService::TestForgeService(Config config)
    : config_(std::move(config)), registry_(&TestRegistry::instance()) {}

TestForgeService::~TestForgeService() = default;

void TestForgeService::initialise() {
    if (initialised_) {
        return;
    }
    config_.validate();

    if (config_.database.enabled) {
        try {
            auto repository = std::make_shared<SqliteResultRepository>(
                config_.database.path, config_.database.busyTimeoutMs);
            repository->initialise();
            repository_ = std::move(repository);
            logger().info("results database ready", {{"path", config_.database.path}});
        } catch (const std::exception& error) {
            // Losing persistence is bad but not fatal: a run that cannot be
            // recorded is still a run whose results the user needs.
            logger().error("persistence disabled for this session",
                           {{"path", config_.database.path}, {"error", std::string(error.what())}});
            repository_.reset();
        }
    }

    if (!diagnostics_) {
        diagnostics_ = diagnostics::DiagnosticCollector::createDefault(config_.diagnostics);
    }
    if (!ai_) {
        ai_ = ai::createProvider(config_.ai);
    }

    initialised_ = true;
}

void TestForgeService::setAiProvider(ai::AiProviderPtr provider) {
    ai_ = std::move(provider);
}

void TestForgeService::setDiagnosticCollector(
    std::shared_ptr<diagnostics::DiagnosticCollector> collector) {
    diagnostics_ = std::move(collector);
}

// ---------------------------------------------------------------------------
// Catalogue
// ---------------------------------------------------------------------------

std::vector<TestMetadata> TestForgeService::listTests(const TestFilter& filter) const {
    TestFilter listing = filter;
    listing.includeDisabled = true;  // listing shows everything; running does not
    return TestSelector::select(*registry_, listing);
}

json::Value TestForgeService::listTestsJson(const TestFilter& filter) const {
    const std::vector<TestMetadata> tests = listTests(filter);

    json::Value out = json::Value::object();
    out.set("ok", true);
    out.set("count", static_cast<std::int64_t>(tests.size()));
    out.set("filter", filter.describe());

    json::Value list = json::Value::array();
    for (const TestMetadata& metadata : tests) {
        list.push(metadata.toJson());
    }
    out.set("tests", list);
    return out;
}

json::Value TestForgeService::suitesJson() const {
    const std::vector<TestMetadata> all = registry_->all();

    // Count per suite so the listing is useful on its own.
    std::map<std::string, int> counts;
    std::map<std::string, int> disabled;
    for (const TestMetadata& metadata : all) {
        ++counts[metadata.suite];
        if (!metadata.enabled) {
            ++disabled[metadata.suite];
        }
    }

    json::Value list = json::Value::array();
    for (const auto& [suite, count] : counts) {
        json::Value entry = json::Value::object();
        entry.set("suite", suite);
        entry.set("tests", count);
        entry.set("disabled", disabled[suite]);
        list.push(entry);
    }

    json::Value tags = json::Value::array();
    for (const std::string& tag : registry_->tags()) {
        tags.push(tag);
    }

    json::Value out = json::Value::object();
    out.set("ok", true);
    out.set("suites", list);
    out.set("tags", tags);
    out.set("total_tests", static_cast<std::int64_t>(all.size()));
    return out;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

bool TestForgeService::runInProgress() const {
    return running_.load(std::memory_order_acquire);
}

void TestForgeService::requestCancellation(const std::string& reason) {
    const std::lock_guard<std::mutex> lock(cancelMutex_);
    if (activeRunner_ != nullptr) {
        activeRunner_->requestCancellation(reason);
    }
}

TestRun TestForgeService::run(const RunOptions& options,
                              const std::vector<std::shared_ptr<RunObserver>>& observers) {
    // One run at a time. Two concurrent runs would share the registry and the
    // results file and produce a report nobody could interpret.
    const std::lock_guard<std::mutex> lock(runMutex_);
    running_.store(true, std::memory_order_release);

    struct RunningGuard {
        std::atomic<bool>* flag;

        ~RunningGuard() { flag->store(false, std::memory_order_release); }
    } guard{&running_};

    TestRunner runner(config_, *registry_);
    {
        const std::lock_guard<std::mutex> cancelLock(cancelMutex_);
        activeRunner_ = &runner;
    }

    struct ActiveRunnerGuard {
        std::mutex* mutex;
        TestRunner** slot;

        ~ActiveRunnerGuard() {
            const std::lock_guard<std::mutex> lock(*mutex);
            *slot = nullptr;
        }
    } runnerGuard{&cancelMutex_, &activeRunner_};

    if (options.persist && repository_) {
        runner.setRepository(repository_);
    }
    if (diagnostics_) {
        runner.setDiagnosticCollector(diagnostics_);
    }
    for (const auto& observer : observers) {
        runner.addObserver(observer);
    }

    TestRun result = runner.run(options);

    const reporting::ReportWriter writer(config_.reporting);
    const std::vector<std::string> reports = writer.write(result);
    if (!reports.empty() && repository_) {
        json::Value payload = json::Value::object();
        json::Value paths = json::Value::array();
        for (const std::string& path : reports) {
            paths.push(path);
        }
        payload.set("reports", paths);
        try {
            repository_->recordEvent(result.runId, "reports_written", payload);
        } catch (const std::exception&) {
            // Already logged by the repository; an event is not worth failing.
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// History and analytics
// ---------------------------------------------------------------------------

json::Value TestForgeService::requireRepository() const {
    if (!repository_) {
        return errorObject(
            "no results database is available (database.enabled is false, or the file could not "
            "be opened)",
            "NO_DATABASE");
    }
    return json::Value{};
}

json::Value TestForgeService::historyJson(int limit) const {
    if (json::Value problem = requireRepository(); !problem.isNull()) {
        return problem;
    }

    RunQuery query;
    query.limit = std::max(1, limit);
    const std::vector<TestRun> runs = repository_->listRuns(query);

    json::Value list = json::Value::array();
    for (const TestRun& run : runs) {
        list.push(run.toSummaryJson());
    }

    json::Value out = json::Value::object();
    out.set("ok", true);
    out.set("count", static_cast<std::int64_t>(runs.size()));
    out.set("runs", list);
    return out;
}

json::Value TestForgeService::statsJson(int runLimit) const {
    if (json::Value problem = requireRepository(); !problem.isNull()) {
        return problem;
    }
    json::Value out = repository_->aggregateStatistics(std::max(1, runLimit));
    out.set("ok", true);
    out.set("storage", repository_->storageInfo());
    return out;
}

json::Value TestForgeService::runJson(const std::string& runId) const {
    if (json::Value problem = requireRepository(); !problem.isNull()) {
        return problem;
    }
    const std::optional<TestRun> run = repository_->loadRun(runId);
    if (!run.has_value()) {
        return errorObject("no run with id '" + runId + "'", "NOT_FOUND");
    }
    json::Value out = run->toJson();
    out.set("ok", true);
    return out;
}

json::Value TestForgeService::runResultsJson(const std::string& runId) const {
    if (json::Value problem = requireRepository(); !problem.isNull()) {
        return problem;
    }
    const std::vector<TestResult> results = repository_->loadResults(runId);
    json::Value list = json::Value::array();
    for (const TestResult& result : results) {
        list.push(result.toJson());
    }
    json::Value out = json::Value::object();
    out.set("ok", true);
    out.set("run_id", runId);
    out.set("count", static_cast<std::int64_t>(results.size()));
    out.set("results", list);
    return out;
}

json::Value TestForgeService::testHistoryJson(const std::string& testId, int limit) const {
    if (json::Value problem = requireRepository(); !problem.isNull()) {
        return problem;
    }
    // Accept either the stable id or the qualified name, because a user
    // reading a report has the name, not the hash.
    std::string resolved = testId;
    if (testId.find('.') != std::string::npos) {
        const std::size_t dot = testId.find('.');
        resolved = ids::testId(testId.substr(0, dot), testId.substr(dot + 1));
    }

    const std::vector<TestHistoryEntry> entries =
        repository_->testHistory(resolved, std::max(1, limit));

    json::Value list = json::Value::array();
    for (const TestHistoryEntry& entry : entries) {
        list.push(entry.toJson());
    }
    json::Value out = json::Value::object();
    out.set("ok", true);
    out.set("test", testId);
    out.set("test_id", resolved);
    out.set("count", static_cast<std::int64_t>(entries.size()));
    out.set("history", list);
    return out;
}

json::Value TestForgeService::flakyCandidatesJson(int limit) const {
    if (json::Value problem = requireRepository(); !problem.isNull()) {
        return problem;
    }
    const std::vector<TestHistorySummary> summaries =
        repository_->historySummaries(std::max(1, limit) * 4);

    json::Value list = json::Value::array();
    for (const TestHistorySummary& summary : summaries) {
        if (summary.looksFlaky()) {
            list.push(summary.toJson());
        }
    }

    json::Value out = json::Value::object();
    out.set("ok", true);
    out.set("count", static_cast<std::int64_t>(list.size()));
    out.set("candidates", list);
    // Stated on the payload itself, not just in the docs: a consumer that only
    // ever sees this JSON still learns not to treat it as a verdict.
    out.set("method",
            "A test is flagged when it has at least 5 non-skipped runs, has both passed and "
            "failed, and changed outcome on at least 20% of consecutive runs. This is a "
            "heuristic: an intermittently broken feature produces the same pattern.");
    return out;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

json::Value TestForgeService::diagnosticsJson() const {
    json::Value out = json::Value::object();
    out.set("ok", true);
    if (!diagnostics_) {
        out.set("ok", false);
        out.set("error", "diagnostics are not configured");
        return out;
    }
    out.set("diagnostics", diagnostics_->collectAll());
    return out;
}

json::Value TestForgeService::gpuJson() const {
    json::Value out = json::Value::object();
    out.set("ok", true);
    if (!diagnostics_) {
        out.set("ok", false);
        out.set("error", "diagnostics are not configured");
        return out;
    }
    out.set("gpu", diagnostics_->gpu().toJson());
    return out;
}

json::Value TestForgeService::healthJson() const {
    json::Value out = json::Value::object();
    out.set("ok", true);
    out.set("service", "testforge");
    out.set("version", Version::string());
    out.set("build", Version::banner());
    out.set("time", toIso8601(WallClock::now()));
    out.set("tests_registered", static_cast<std::int64_t>(registry_->size()));
    out.set("run_in_progress", runInProgress());

    json::Value components = json::Value::object();
    components.set("database", repository_ ? "ready" : "disabled");
    components.set("diagnostics", diagnostics_ ? "ready" : "disabled");
    components.set("ai", ai_ && ai_->isEnabled() ? "enabled" : "disabled");
    out.set("components", components);
    return out;
}

// ---------------------------------------------------------------------------
// AI
// ---------------------------------------------------------------------------

json::Value TestForgeService::generateTests(const std::string& requirement,
                                            const std::string& suiteName,
                                            int maxTests,
                                            bool registerSuite) {
    if (!ai_) {
        return errorObject("no AI provider is configured", "AI_UNAVAILABLE");
    }
    if (strings::trim(requirement).empty()) {
        return errorObject("the requirement text is empty", "INVALID_REQUEST");
    }
    if (requirement.size() > 8000) {
        return errorObject("the requirement text exceeds 8000 characters", "INVALID_REQUEST");
    }

    ai::GenerationRequest request;
    request.requirement = requirement;
    request.suiteName = suiteName.empty() ? "generated" : suiteName;
    request.baseUrl = config_.api.baseUrl;
    request.maxTests = maxTests > 0 ? maxTests : config_.ai.maxGeneratedTests;

    ai::GenerationResult generation = ai_->generateTests(request);
    if (!generation.ok) {
        json::Value out = errorObject(generation.error, "GENERATION_FAILED");
        out.set("provider", generation.provider);
        if (!ai_->isEnabled()) {
            out.set("hint", ai_->health());
        }
        return out;
    }

    // Validation is not optional and not skippable. Everything below this line
    // treats the suite as trusted precisely because it passed here.
    net::UrlPolicy policy = net::UrlPolicy::restrictedTo(config_.api.baseUrl);
    for (const std::string& host : config_.ai.allowedTargetHosts) {
        policy.allowedHosts.push_back(host);
    }
    const ai::SpecValidator validator(config_.ai, policy, config_.api.baseUrl);
    const ai::ValidationReport report = validator.validate(generation.suite);

    json::Value out = json::Value::object();
    out.set("ok", report.valid);
    out.set("provider", generation.provider);
    out.set("model", generation.model);
    out.set("elapsed_ms", millisOf(generation.elapsed));
    out.set("validation", report.toJson());
    out.set("specification", generation.suite.toJson());
    out.set("usage",
            json::Value::object()
                .set("prompt_tokens", generation.promptTokens)
                .set("completion_tokens", generation.completionTokens));

    if (!report.valid) {
        out.set("error", "every generated test was rejected by validation");
        out.set("code", "VALIDATION_REJECTED");
        return out;
    }

    if (registerSuite) {
        try {
            const std::size_t count = registerSpecSuite(*registry_, generation.suite, config_);
            out.set("registered", static_cast<std::int64_t>(count));
            out.set("suite", generation.suite.suite);
            logger().info("registered a generated suite",
                          {{"suite", generation.suite.suite},
                           {"tests", static_cast<std::int64_t>(count)},
                           {"model", generation.model}});
        } catch (const std::exception& error) {
            out.set("ok", false);
            out.set("error", std::string("registration refused: ") + error.what());
            out.set("code", "REGISTRATION_REFUSED");
        }
    } else {
        out.set("registered", 0);
    }

    return out;
}

json::Value TestForgeService::analyseResult(const TestResult& result) {
    if (!ai_) {
        return errorObject("no AI provider is configured", "AI_UNAVAILABLE");
    }
    if (!isFailure(result.status)) {
        return errorObject("only failing results can be analysed", "INVALID_REQUEST");
    }

    const ai::FailureContext builder;
    json::Value context = builder.build(result);

    if (repository_) {
        // Recent history is what separates "newly broken" from "always broken".
        const std::vector<TestHistoryEntry> history = repository_->testHistory(result.testId, 10);
        json::Value entries = json::Value::array();
        for (const TestHistoryEntry& entry : history) {
            entries.push(entry.toJson());
        }
        context = builder.withHistory(std::move(context), entries);
    }

    ai::AnalysisRequest request;
    request.context = context;
    request.testName = result.qualifiedName();
    request.failureCategory = std::string(toString(result.failureCategory));

    const ai::AnalysisResult analysis = ai_->analyseFailure(request);

    json::Value out = json::Value::object();
    out.set("ok", analysis.ok);
    out.set("test", result.qualifiedName());
    // The engine's verdict is repeated next to the AI's opinion so that no
    // consumer can render one without the other.
    out.set("verdict",
            json::Value::object()
                .set("status", std::string(toString(result.status)))
                .set("failure_category", std::string(toString(result.failureCategory)))
                .set("decided_by", "testforge-engine"));
    out.set("analysis", analysis.toJson());
    if (!analysis.ok) {
        out.set("error", analysis.error);
    }

    if (repository_ && analysis.ok && !result.runId.empty()) {
        try {
            repository_->recordEvent(result.runId, "ai_analysis", out);
        } catch (const std::exception&) {
            // Non-fatal: the analysis is still returned to the caller.
        }
    }

    return out;
}

json::Value TestForgeService::analyseFailure(const std::string& runId,
                                             const std::string& testName) {
    if (json::Value problem = requireRepository(); !problem.isNull()) {
        return problem;
    }

    const std::vector<TestResult> results = repository_->loadResults(runId);
    if (results.empty()) {
        return errorObject("no results for run '" + runId + "'", "NOT_FOUND");
    }

    const TestResult* chosen = nullptr;
    for (const TestResult& result : results) {
        if (!testName.empty()) {
            if (result.qualifiedName() == testName || result.testName == testName) {
                chosen = &result;
                break;
            }
        } else if (isFailure(result.status)) {
            // No name given: analyse the first failure, which is what somebody
            // triaging a run actually wants.
            chosen = &result;
            break;
        }
    }

    if (chosen == nullptr) {
        return errorObject(testName.empty()
                               ? "run '" + runId + "' contains no failures to analyse"
                               : "run '" + runId + "' has no test named '" + testName + "'",
                           "NOT_FOUND");
    }

    return analyseResult(*chosen);
}

json::Value TestForgeService::loadSpecFile(const std::string& path, bool registerSuite) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return errorObject("cannot open specification file: " + path, "NOT_FOUND");
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    std::string error;
    std::optional<ai::TestSpecSuite> suite = ai::TestSpecSuite::parse(buffer.str(), &error);
    if (!suite.has_value()) {
        return errorObject(error, "INVALID_SPEC");
    }
    if (suite->source.empty()) {
        suite->source = "file:" + path;
    }

    // A file on disk gets exactly the same scrutiny as model output. It is
    // still input, and the person who wrote it is still capable of a typo.
    net::UrlPolicy policy = net::UrlPolicy::restrictedTo(config_.api.baseUrl);
    for (const std::string& host : config_.ai.allowedTargetHosts) {
        policy.allowedHosts.push_back(host);
    }
    const ai::SpecValidator validator(config_.ai, policy, config_.api.baseUrl);
    const ai::ValidationReport report = validator.validate(*suite);

    json::Value out = json::Value::object();
    out.set("ok", report.valid);
    out.set("path", path);
    out.set("validation", report.toJson());
    out.set("specification", suite->toJson());

    if (report.valid && registerSuite) {
        try {
            const std::size_t count = registerSpecSuite(*registry_, *suite, config_);
            out.set("registered", static_cast<std::int64_t>(count));
            out.set("suite", suite->suite);
        } catch (const std::exception& registrationError) {
            out.set("ok", false);
            out.set("error", std::string("registration refused: ") + registrationError.what());
        }
    }
    return out;
}

}  // namespace testforge::api
