#include "Commands.hpp"

#include "CliOutput.hpp"

#include "testforge/ai/MockAiProvider.hpp"
#include "testforge/api/RestApiServer.hpp"
#include "testforge/core/Exceptions.hpp"
#include "testforge/core/Interrupt.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/core/Version.hpp"
#include "testforge/execution/FailureClassifier.hpp"
#include "testforge/reporting/Reporter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace testforge::cli {
namespace {

bool wantsJson(const ParsedCommand& parsed) {
    return parsed.flag("json");
}

int emitJson(const json::Value& value) {
    CliOutput::json(std::cout, value);
    const json::Value* ok = value.find("ok");
    return (ok != nullptr && !ok->boolOr(true)) ? exitcode::kUsageError : exitcode::kSuccess;
}

std::string statusText(TestStatus status) {
    const std::string text(toString(status));
    switch (status) {
        case TestStatus::Passed:
            return CliOutput::green(text);
        case TestStatus::Failed:
        case TestStatus::Error:
            return CliOutput::red(text);
        case TestStatus::Timeout:
            return CliOutput::yellow(text);
        case TestStatus::Skipped:
            return CliOutput::dim(text);
    }
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// list
// ---------------------------------------------------------------------------

int Commands::list(api::TestForgeService& service, const ParsedCommand& parsed) {
    const TestFilter filter = CommandLine::filterFrom(parsed);

    if (wantsJson(parsed)) {
        return emitJson(service.listTestsJson(filter));
    }

    const std::vector<TestMetadata> tests = service.listTests(filter);
    if (tests.empty()) {
        std::cout << "No tests match " << filter.describe() << ".\n";
        if (service.registry().size() > 0) {
            std::cout << CliOutput::dim("There are " + std::to_string(service.registry().size()) +
                                        " registered tests; check the filter.\n");
        }
        return exitcode::kSuccess;
    }

    std::vector<std::vector<std::string>> rows;
    rows.reserve(tests.size());
    for (const TestMetadata& metadata : tests) {
        std::vector<std::string> tags = metadata.tags;
        // The suite is added as an implicit tag at registration; showing it in
        // both columns is just noise.
        tags.erase(std::remove(tags.begin(), tags.end(), metadata.suite), tags.end());

        rows.push_back({metadata.suite,
                        metadata.name,
                        strings::join(tags, ","),
                        metadata.timeoutMs > 0 ? std::to_string(metadata.timeoutMs) + "ms" : "-",
                        metadata.enabled ? "" : CliOutput::yellow("disabled"),
                        strings::truncate(metadata.description, 60)});
    }

    CliOutput::table(std::cout, {"SUITE", "TEST", "TAGS", "TIMEOUT", "STATE", "DESCRIPTION"}, rows);
    std::cout << '\n' << tests.size() << " test(s)";
    const std::vector<std::string> suites = service.registry().suites();
    std::cout << " in " << suites.size() << " suite(s): " << strings::join(suites, ", ") << '\n';
    return exitcode::kSuccess;
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

int Commands::run(api::TestForgeService& service, const ParsedCommand& parsed) {
    RunOptions options;
    options.filter = CommandLine::filterFrom(parsed);
    options.selection = CommandLine::selectionFrom(parsed);

    // A bare `testforge run` skips the suites configured as opt-in (the
    // failure-injection suite, whose tests are supposed to fail). Naming any
    // filter explicitly means the user knows what they are asking for, so the
    // exclusion is dropped.
    if (options.filter.empty()) {
        options.filter.excludeSuites = service.config().execution.excludeSuitesByDefault;
    }
    options.workers = parsed.intValue("workers", 0);
    options.failFast = parsed.flag("fail-fast", service.config().execution.failFast);
    options.retryFailed = parsed.intValue("retry-failed", service.config().execution.retryFailed);
    options.label = parsed.value("label");
    options.persist = !parsed.flag("no-persist");

    if (parsed.flag("dry-run")) {
        const std::vector<TestMetadata> selected =
            TestSelector::select(service.registry(), options.filter, options.selection);
        std::cout << "Would run " << selected.size() << " test(s):\n";
        for (const TestMetadata& metadata : selected) {
            std::cout << "  " << metadata.qualifiedName() << '\n';
        }
        return exitcode::kSuccess;
    }

    const std::vector<TestMetadata> selected =
        TestSelector::select(service.registry(), options.filter, options.selection);
    if (selected.empty()) {
        CliOutput::error("no tests match " + options.filter.describe(),
                         "run 'testforge list' to see what is registered");
        return exitcode::kUsageError;
    }

    reporting::ConsoleReporterOptions consoleOptions;
    consoleOptions.color = CliOutput::colorEnabled();
    consoleOptions.verboseFailures = service.config().reporting.verboseFailures;
    consoleOptions.live = !wantsJson(parsed);
    auto console = std::make_shared<reporting::ConsoleReporter>(consoleOptions);

    std::vector<std::shared_ptr<RunObserver>> observers;
    if (service.config().reporting.console && !wantsJson(parsed)) {
        observers.push_back(console);
    }

    // Ctrl-C during a run must stop it, not be swallowed. The run happens on
    // this thread, so something else has to notice the flag: a watcher that
    // polls it and asks the service to stop starting new tests. Results
    // already collected are kept and still reported — an interrupted run that
    // threw away its findings would be worse than useless on a long suite.
    std::atomic<bool> runFinished{false};
    std::thread interruptWatcher([&service, &runFinished] {
        while (!runFinished.load(std::memory_order_acquire)) {
            if (interrupt::requested()) {
                service.requestCancellation("interrupted by signal");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    const TestRun result = service.run(options, observers);

    runFinished.store(true, std::memory_order_release);
    interruptWatcher.join();

    if (wantsJson(parsed)) {
        json::Value out = result.toJson();
        out.set("ok", result.exitCode() == 0);
        CliOutput::json(std::cout, out);
    }

    // 1 for test failures, 3 for a framework fault: CI treats them differently.
    const int code = result.exitCode();
    if (code == 2) {
        return exitcode::kFrameworkError;
    }
    if (code == 4) {
        // Interrupted. 128 + signal is the shell convention, and it is
        // distinct from both "passed" and "tests failed" — an interrupted run
        // verified nothing about the tests it never reached.
        const int signalNumber = interrupt::signalNumber();
        return signalNumber != 0 ? 128 + signalNumber : exitcode::kTestsFailed;
    }
    return code == 0 ? exitcode::kSuccess : exitcode::kTestsFailed;
}

// ---------------------------------------------------------------------------
// report
// ---------------------------------------------------------------------------

int Commands::report(api::TestForgeService& service, const ParsedCommand& parsed) {
    std::string runId = parsed.value("run");
    if (runId.empty() && !parsed.positionals.empty()) {
        runId = parsed.positionals.front();
    }

    if (runId.empty() || runId == "latest") {
        const json::Value history = service.historyJson(1);
        const json::Value* runs = history.find("runs");
        if (runs == nullptr || !runs->isArray() || runs->asArray().empty()) {
            CliOutput::error("no stored runs to report on", "run some tests first: testforge run");
            return exitcode::kUsageError;
        }
        runId = json::stringAt(runs->asArray().front(), "run_id");
    }

    const json::Value document = service.runJson(runId);
    if (document.find("ok") == nullptr || !json::boolAt(document, "ok", false)) {
        CliOutput::error(document.find("error") != nullptr
                             ? json::stringAt(document, "error", "unknown error")
                             : "could not load the run");
        return exitcode::kUsageError;
    }

    const TestRun run = TestRun::fromJson(document);
    const std::string format = strings::toLower(parsed.value("format", "console"));

    std::unique_ptr<reporting::Reporter> reporter;
    if (format == "console" || format == "text") {
        reporting::ConsoleReporterOptions options;
        options.color = CliOutput::colorEnabled();
        options.live = false;
        reporter = std::make_unique<reporting::ConsoleReporter>(options);
    } else if (format == "json") {
        reporter = std::make_unique<reporting::JsonReporter>(true);
    } else if (format == "html") {
        reporter = std::make_unique<reporting::HtmlReporter>();
    } else if (format == "junit" || format == "xml") {
        reporter = std::make_unique<reporting::JUnitReporter>();
    } else {
        CliOutput::error("unknown report format '" + format + "'",
                         "supported: console, json, html, junit");
        return exitcode::kUsageError;
    }

    const std::string rendered = reporter->render(run);
    const std::string output = parsed.value("output");
    if (output.empty()) {
        std::cout << rendered;
        return exitcode::kSuccess;
    }

    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        CliOutput::error("cannot write to " + output);
        return exitcode::kFrameworkError;
    }
    file << rendered;
    std::cout << "Report written to " << output << '\n';
    return exitcode::kSuccess;
}

// ---------------------------------------------------------------------------
// diagnose
// ---------------------------------------------------------------------------

int Commands::diagnose(api::TestForgeService& service, const ParsedCommand& parsed) {
    if (parsed.flag("gpu")) {
        const json::Value document = service.gpuJson();
        if (wantsJson(parsed)) {
            return emitJson(document);
        }
        const json::Value* gpu = document.find("gpu");
        if (gpu == nullptr) {
            CliOutput::error("GPU diagnostics are unavailable");
            return exitcode::kFrameworkError;
        }
        CliOutput::heading(std::cout, "GPU");
        std::cout << "  " << json::stringAt(*gpu, "summary", "") << '\n';
        if (gpu->find("is_mock_data") != nullptr && json::boolAt(*gpu, "is_mock_data", false)) {
            CliOutput::warning("the values below are MOCK TEST DATA, not real hardware readings");
        }
        if (json::boolAt(*gpu, "available", false)) {
            const json::Value* devices = gpu->find("devices");
            std::vector<std::vector<std::string>> rows;
            for (const json::Value& device : devices->asArray()) {
                rows.push_back(
                    {std::to_string(json::intAt(device, "index", 0)),
                     json::stringAt(device, "name", "?"),
                     device.find("memory_used_mb") != nullptr
                         ? std::to_string(json::intAt(device, "memory_used_mb", 0)) + " / " +
                               std::to_string(json::intAt(device, "memory_total_mb", 0)) + " MB"
                         : "-",
                     device.find("utilization_percent") != nullptr
                         ? std::to_string(
                               static_cast<int>(json::doubleAt(device, "utilization_percent", 0))) +
                               "%"
                         : "-",
                     device.find("temperature_celsius") != nullptr
                         ? std::to_string(
                               static_cast<int>(json::doubleAt(device, "temperature_celsius", 0))) +
                               "C"
                         : "-"});
            }
            std::cout << '\n';
            CliOutput::table(std::cout,
                             {"IDX", "NAME", "MEMORY", "UTIL", "TEMP"},
                             rows,
                             {true, false, true, true, true});
        }
        return exitcode::kSuccess;
    }

    const json::Value document = service.diagnosticsJson();
    if (wantsJson(parsed) || parsed.flag("full")) {
        return emitJson(document);
    }

    const json::Value* diagnostics = document.find("diagnostics");
    if (diagnostics == nullptr) {
        CliOutput::error("diagnostics are unavailable");
        return exitcode::kFrameworkError;
    }

    const auto show = [&diagnostics](std::string_view path, std::string_view label) {
        if (const json::Value* value = diagnostics->path(path); value != nullptr) {
            std::string text;
            if (value->isString()) {
                text = value->asString();
            } else if (value->isNumber()) {
                if (value->isInteger()) {
                    text = std::to_string(value->asInt());
                } else {
                    // Two decimals: std::to_string gives six, which reads as
                    // noise for a load average or a percentage.
                    std::ostringstream number;
                    number.setf(std::ios::fixed);
                    number.precision(2);
                    number << value->asDouble();
                    text = number.str();
                }
            } else if (value->isBool()) {
                text = value->asBool() ? "yes" : "no";
            } else {
                return;
            }
            CliOutput::keyValue(std::cout, label, text, 22);
        }
    };

    CliOutput::heading(std::cout, "Operating system");
    show("system.os.distribution", "distribution");
    show("system.os.kernel_version", "kernel");
    show("system.os.architecture", "architecture");
    show("system.os.hostname", "hostname");
    show("system.os.uptime_seconds", "uptime (s)");
    show("system.os.inside_container", "in container");
    show("system.os.container_hint", "container hint");

    CliOutput::heading(std::cout, "CPU");
    show("system.cpu.model", "model");
    show("system.cpu.logical_cores", "logical cores");
    show("system.cpu.physical_cores", "physical cores");
    show("system.cpu.load_average_1m", "load (1m)");
    show("system.cpu.utilization_percent", "utilization %");

    CliOutput::heading(std::cout, "Memory");
    show("system.memory.total_kb", "total (kB)");
    show("system.memory.available_kb", "available (kB)");
    show("system.memory.used_percent", "used %");
    show("system.memory.process_rss_kb", "testforge rss (kB)");

    if (const json::Value* disks = diagnostics->path("system.disks");
        disks != nullptr && disks->isArray() && !disks->asArray().empty()) {
        CliOutput::heading(std::cout, "Filesystems");
        std::vector<std::vector<std::string>> rows;
        for (const json::Value& disk : disks->asArray()) {
            const std::int64_t total =
                disk.find("total_bytes") != nullptr ? json::intAt(disk, "total_bytes", 0) : 0;
            const std::int64_t available = disk.find("available_bytes") != nullptr
                                               ? json::intAt(disk, "available_bytes", 0)
                                               : 0;
            rows.push_back(
                {json::stringAt(disk, "mount_point", "?"),
                 std::to_string(total / (1024 * 1024 * 1024)) + " GB",
                 std::to_string(available / (1024 * 1024 * 1024)) + " GB",
                 disk.find("used_percent") != nullptr
                     ? std::to_string(static_cast<int>(json::doubleAt(disk, "used_percent", 0))) +
                           "%"
                     : "-"});
        }
        CliOutput::table(
            std::cout, {"MOUNT", "TOTAL", "FREE", "USED"}, rows, {false, true, true, true});
    }

    if (const json::Value* interfaces = diagnostics->path("system.network_interfaces");
        interfaces != nullptr && interfaces->isArray()) {
        CliOutput::heading(std::cout, "Network");
        for (const json::Value& item : interfaces->asArray()) {
            if (!json::boolAt(item, "up", false)) {
                continue;
            }
            std::vector<std::string> addresses;
            for (const json::Value& address : json::arrayAt(item, "addresses")) {
                addresses.push_back(address.stringOr(""));
            }
            CliOutput::keyValue(
                std::cout, json::stringAt(item, "name", "?"), strings::join(addresses, ", "), 22);
        }
    }

    CliOutput::heading(std::cout, "GPU");
    if (const json::Value* summary = diagnostics->path("gpu.summary");
        summary != nullptr && summary->isString()) {
        std::cout << "  " << summary->asString() << '\n';
    } else {
        std::cout << "  GPU diagnostics were not collected.\n";
    }

    std::cout << '\n' << CliOutput::dim("Use --full or --json for the complete document.") << '\n';
    return exitcode::kSuccess;
}

// ---------------------------------------------------------------------------
// history
// ---------------------------------------------------------------------------

int Commands::history(api::TestForgeService& service, const ParsedCommand& parsed) {
    if (parsed.flag("prune")) {
        return database(service, parsed);
    }

    const int limit = parsed.intValue("limit", 20);
    const std::string test = parsed.value("test");

    if (!test.empty()) {
        const json::Value document = service.testHistoryJson(test, limit);
        if (wantsJson(parsed)) {
            return emitJson(document);
        }
        if (!json::boolAt(document, "ok", false)) {
            CliOutput::error(json::stringAt(document, "error", "unknown error"));
            return exitcode::kUsageError;
        }
        CliOutput::heading(std::cout, "History for " + test);
        std::vector<std::vector<std::string>> rows;
        for (const json::Value& entry : json::arrayAt(document, "history")) {
            const std::optional<TestStatus> status =
                testStatusFromString(json::stringAt(entry, "status", ""));
            rows.push_back({json::stringAt(entry, "start_time", ""),
                            status.has_value() ? statusText(*status) : "?",
                            std::to_string(json::intAt(entry, "duration_ms", 0)) + "ms",
                            json::stringAt(entry, "failure_category", ""),
                            strings::truncate(json::stringAt(entry, "error_message", ""), 60)});
        }
        CliOutput::table(std::cout,
                         {"WHEN", "STATUS", "DURATION", "CATEGORY", "MESSAGE"},
                         rows,
                         {false, false, true, false, false});
        return exitcode::kSuccess;
    }

    const json::Value document = service.historyJson(limit);
    if (wantsJson(parsed)) {
        return emitJson(document);
    }
    if (!json::boolAt(document, "ok", false)) {
        CliOutput::error(json::stringAt(document, "error", "unknown error"));
        return exitcode::kUsageError;
    }

    const json::Value* runs = document.find("runs");
    if (runs->asArray().empty()) {
        std::cout << "No runs recorded yet.\n";
        return exitcode::kSuccess;
    }

    std::vector<std::vector<std::string>> rows;
    for (const json::Value& run : runs->asArray()) {
        const json::Value* stats = run.find("statistics");
        const double rate = json::doubleAt(*stats, "success_rate", 0.0);
        std::ostringstream rateText;
        rateText.setf(std::ios::fixed);
        rateText.precision(1);
        rateText << rate << '%';

        rows.push_back({json::stringAt(run, "run_id", "").substr(0, 8),
                        json::stringAt(run, "started_at", ""),
                        std::to_string(json::intAt(*stats, "total", 0)),
                        CliOutput::green(std::to_string(json::intAt(*stats, "passed", 0))),
                        CliOutput::red(std::to_string(json::intAt(*stats, "failed", 0))),
                        std::to_string(json::intAt(*stats, "skipped", 0)),
                        rate >= 99.5 ? CliOutput::green(rateText.str())
                                     : (rate >= 90.0 ? CliOutput::yellow(rateText.str())
                                                     : CliOutput::red(rateText.str())),
                        formatDuration(Milliseconds{json::intAt(run, "duration_ms", 0)}),
                        strings::truncate(json::stringAt(run, "filter", ""), 30)});
    }

    CliOutput::table(std::cout,
                     {"RUN", "STARTED", "TOTAL", "PASS", "FAIL", "SKIP", "RATE", "TIME", "FILTER"},
                     rows,
                     {false, false, true, true, true, true, true, true, false});
    return exitcode::kSuccess;
}

// ---------------------------------------------------------------------------
// stats
// ---------------------------------------------------------------------------

int Commands::stats(api::TestForgeService& service, const ParsedCommand& parsed) {
    if (parsed.flag("flaky")) {
        const json::Value document = service.flakyCandidatesJson(parsed.intValue("limit", 20));
        if (wantsJson(parsed)) {
            return emitJson(document);
        }
        CliOutput::heading(std::cout, "Flaky candidates (heuristic)");
        const json::Value* candidates = document.find("candidates");
        if (candidates == nullptr || candidates->asArray().empty()) {
            std::cout << "  None identified.\n";
        } else {
            std::vector<std::vector<std::string>> rows;
            for (const json::Value& item : candidates->asArray()) {
                std::ostringstream flip;
                flip.setf(std::ios::fixed);
                flip.precision(2);
                flip << json::doubleAt(item, "flip_rate", 0.0);
                rows.push_back({json::stringAt(item, "test", ""),
                                std::to_string(json::intAt(item, "total_runs", 0)),
                                std::to_string(json::intAt(item, "passes", 0)),
                                std::to_string(json::intAt(item, "failures", 0)),
                                flip.str()});
            }
            CliOutput::table(std::cout,
                             {"TEST", "RUNS", "PASS", "FAIL", "FLIP RATE"},
                             rows,
                             {false, true, true, true, true});
        }
        std::cout << '\n' << CliOutput::dim(json::stringAt(document, "method", "")) << '\n';
        return exitcode::kSuccess;
    }

    const json::Value document = service.statsJson(parsed.intValue("runs", 50));
    if (wantsJson(parsed)) {
        return emitJson(document);
    }
    if (!json::boolAt(document, "ok", false)) {
        CliOutput::error(json::stringAt(document, "error", "unknown error"));
        return exitcode::kUsageError;
    }

    CliOutput::heading(std::cout, "Aggregate statistics");
    const auto number = [&document](std::string_view key) {
        const json::Value* value = document.find(key);
        return value != nullptr ? std::to_string(value->intOr(0)) : "0";
    };
    CliOutput::keyValue(std::cout, "runs analysed", number("runs"));
    CliOutput::keyValue(std::cout, "tests executed", number("total_tests"));
    CliOutput::keyValue(std::cout, "passed", number("passed"));
    CliOutput::keyValue(std::cout, "failed", number("failed"));
    CliOutput::keyValue(std::cout, "skipped", number("skipped"));
    CliOutput::keyValue(std::cout, "errors", number("errors"));
    CliOutput::keyValue(std::cout, "timeouts", number("timeouts"));
    {
        std::ostringstream rate;
        rate.setf(std::ios::fixed);
        rate.precision(2);
        rate << json::doubleAt(document, "success_rate", 0.0) << '%';
        CliOutput::keyValue(std::cout, "success rate", rate.str());
    }
    CliOutput::keyValue(
        std::cout,
        "avg run duration",
        formatDuration(Milliseconds{json::intAt(document, "average_run_duration_ms", 0)}));

    if (const json::Value* categories = document.find("failure_categories");
        categories != nullptr && !categories->asArray().empty()) {
        CliOutput::heading(std::cout, "Failure categories");
        std::vector<std::vector<std::string>> rows;
        for (const json::Value& entry : categories->asArray()) {
            const std::optional<FailureCategory> category =
                failureCategoryFromString(json::stringAt(entry, "category", ""));
            rows.push_back(
                {json::stringAt(entry, "category", ""),
                 std::to_string(json::intAt(entry, "count", 0)),
                 category.has_value() ? std::string(FailureClassifier::explain(*category)) : ""});
        }
        CliOutput::table(std::cout, {"CATEGORY", "COUNT", "MEANING"}, rows, {false, true, false});
    }

    if (const json::Value* tests = document.find("top_failing_tests");
        tests != nullptr && !tests->asArray().empty()) {
        CliOutput::heading(std::cout, "Top failing tests");
        std::vector<std::vector<std::string>> rows;
        for (const json::Value& entry : tests->asArray()) {
            rows.push_back({json::stringAt(entry, "test", ""),
                            std::to_string(json::intAt(entry, "failures", 0))});
        }
        CliOutput::table(std::cout, {"TEST", "FAILURES"}, rows, {false, true});
    }

    if (const json::Value* slow = document.find("slowest_tests");
        slow != nullptr && !slow->asArray().empty()) {
        CliOutput::heading(std::cout, "Slowest tests");
        std::vector<std::vector<std::string>> rows;
        for (const json::Value& entry : slow->asArray()) {
            rows.push_back(
                {json::stringAt(entry, "test", ""),
                 formatDuration(Milliseconds{json::intAt(entry, "average_duration_ms", 0)})});
        }
        CliOutput::table(std::cout, {"TEST", "AVG DURATION"}, rows, {false, true});
    }

    return exitcode::kSuccess;
}

// ---------------------------------------------------------------------------
// serve
// ---------------------------------------------------------------------------

int Commands::serve(api::TestForgeService& service, const ParsedCommand& parsed) {
    ServerConfig config = service.config().server;
    config.host = parsed.value("host", config.host);
    config.port = parsed.intValue("port", config.port);
    config.workers = parsed.intValue("workers", config.workers);
    config.staticDirectory = parsed.value("static", config.staticDirectory);

    api::RestApiServer server(service, config);
    if (!server.start()) {
        CliOutput::error(
            "could not start the server on " + config.host + ":" + std::to_string(config.port),
            "the port may already be in use; try --port 0 for any free port");
        return exitcode::kFrameworkError;
    }

    std::cout << CliOutput::bold("TestForge server") << '\n';
    CliOutput::keyValue(std::cout, "api", server.address() + "/api");
    CliOutput::keyValue(std::cout, "dashboard", server.address() + "/");
    CliOutput::keyValue(std::cout, "static files", config.staticDirectory);
    std::cout << '\n' << CliOutput::dim("Press Ctrl-C to stop.") << '\n';

    // Poll rather than block outright. A signal handler can only set a flag,
    // and a condition_variable wait is not woken by a signal, so blocking in
    // server.wait() would make the line printed above a lie: neither Ctrl-C
    // nor `docker stop` could ever end the process.
    //
    // 200 ms is short enough to feel immediate and long enough to be free.
    while (!interrupt::requested()) {
        if (server.waitFor(Milliseconds{200})) {
            return exitcode::kSuccess;  // stopped by something else
        }
    }

    std::cout << '\n' << CliOutput::dim("stopping") << '\n';
    server.stop();
    return exitcode::kSuccess;
}

// ---------------------------------------------------------------------------
// ai
// ---------------------------------------------------------------------------

int Commands::ai(api::TestForgeService& service, const ParsedCommand& parsed) {
    if (parsed.flag("mock")) {
        // Explicit, never a silent fallback: the user asked for the offline
        // stand-in, so the output can be labelled as such.
        service.setAiProvider(std::make_shared<testforge::ai::MockAiProvider>());
    }

    const std::string subcommand =
        parsed.subcommand.empty() ? std::string("status") : parsed.subcommand;

    if (subcommand == "status") {
        json::Value out = json::Value::object();
        out.set("ok", true);
        out.set("ai",
                service.aiProvider() ? service.aiProvider()->health()
                                     : json::Value::object().set("enabled", false));
        if (wantsJson(parsed)) {
            return emitJson(out);
        }
        CliOutput::heading(std::cout, "AI provider");
        const json::Value* health = out.find("ai");
        for (const json::Member& member : health->asObject()) {
            std::string text;
            if (member.second.isString()) {
                text = member.second.asString();
            } else if (member.second.isBool()) {
                text = member.second.asBool() ? "yes" : "no";
            } else if (member.second.isNumber()) {
                text = std::to_string(member.second.intOr(0));
            } else {
                text = member.second.dump();
            }
            CliOutput::keyValue(std::cout, member.first, text, 22);
        }
        return exitcode::kSuccess;
    }

    if (subcommand == "generate-tests" || subcommand == "generate") {
        std::string requirement = parsed.value("requirement");
        if (requirement.empty() && !parsed.positionals.empty()) {
            requirement = strings::join(parsed.positionals, " ");
        }
        if (const std::string file = parsed.value("file"); !file.empty()) {
            std::ifstream input(file);
            if (!input.is_open()) {
                CliOutput::error("cannot read " + file);
                return exitcode::kUsageError;
            }
            std::ostringstream buffer;
            buffer << input.rdbuf();
            requirement = buffer.str();
        }
        if (strings::trim(requirement).empty()) {
            CliOutput::error("no requirement supplied",
                             "testforge ai generate-tests \"Create tests for the users API\"");
            return exitcode::kUsageError;
        }

        const json::Value out = service.generateTests(requirement,
                                                      parsed.value("suite", "generated"),
                                                      parsed.intValue("max-tests", 10),
                                                      parsed.flag("register"));
        if (wantsJson(parsed)) {
            return emitJson(out);
        }

        const bool ok = json::boolAt(out, "ok", false);
        CliOutput::heading(std::cout, "AI test generation");
        CliOutput::keyValue(std::cout, "provider", json::stringAt(out, "provider", "?"), 22);
        CliOutput::keyValue(std::cout, "model", json::stringAt(out, "model", "?"), 22);

        if (!ok) {
            CliOutput::error(json::stringAt(out, "error", "generation failed"));
            if (const json::Value* validation = out.find("validation"); validation != nullptr) {
                for (const json::Value& issue : json::arrayAt(*validation, "issues")) {
                    std::cerr << "  - " << json::stringAt(issue, "where", "") << ": "
                              << json::stringAt(issue, "message", "") << '\n';
                }
            }
            return exitcode::kUsageError;
        }

        const json::Value* validation = out.find("validation");
        CliOutput::keyValue(
            std::cout, "accepted", std::to_string(json::intAt(*validation, "accepted", 0)), 22);
        CliOutput::keyValue(
            std::cout, "rejected", std::to_string(json::intAt(*validation, "rejected", 0)), 22);
        if (out.find("registered") != nullptr) {
            CliOutput::keyValue(
                std::cout, "registered", std::to_string(json::intAt(out, "registered", 0)), 22);
        }

        if (const json::Value* issues = validation->find("issues");
            issues != nullptr && !issues->asArray().empty()) {
            CliOutput::heading(std::cout, "Validation issues");
            for (const json::Value& issue : issues->asArray()) {
                const bool fatal = json::boolAt(issue, "fatal", true);
                std::cout << "  "
                          << (fatal ? CliOutput::red("rejected") : CliOutput::yellow("warn"))
                          << "  " << json::stringAt(issue, "where", "") << ": "
                          << json::stringAt(issue, "message", "") << '\n';
            }
        }

        CliOutput::heading(std::cout, "Generated tests");
        std::vector<std::vector<std::string>> rows;
        for (const json::Value& test : json::arrayAt(out, "specification.tests")) {
            rows.push_back({json::stringAt(test, "name", ""),
                            json::stringAt(test, "method", ""),
                            json::stringAt(test, "endpoint", ""),
                            std::to_string(json::intAt(test, "expected_status", 0)),
                            strings::truncate(json::stringAt(test, "description", ""), 50)});
        }
        CliOutput::table(std::cout,
                         {"NAME", "METHOD", "ENDPOINT", "EXPECT", "DESCRIPTION"},
                         rows,
                         {false, false, false, true, false});

        if (parsed.flag("register")) {
            std::cout << '\n'
                      << "Run them with: testforge run --suite "
                      << json::stringAt(out, "specification.suite", "generated") << '\n';
        } else {
            std::cout << '\n'
                      << CliOutput::dim("Add --register to make these runnable in this process.")
                      << '\n';
        }
        return exitcode::kSuccess;
    }

    if (subcommand == "analyze-failure" || subcommand == "analyse-failure" ||
        subcommand == "analyze") {
        std::string runId = parsed.value("run");
        if (runId.empty() && !parsed.positionals.empty()) {
            runId = parsed.positionals.front();
        }
        if (runId.empty() || runId == "latest") {
            const json::Value history = service.historyJson(1);
            const json::Value* runs = history.find("runs");
            if (runs == nullptr || !runs->isArray() || runs->asArray().empty()) {
                CliOutput::error("no stored runs to analyse");
                return exitcode::kUsageError;
            }
            runId = json::stringAt(runs->asArray().front(), "run_id");
        }

        const json::Value out = service.analyseFailure(runId, parsed.value("test"));
        if (wantsJson(parsed)) {
            return emitJson(out);
        }
        if (!json::boolAt(out, "ok", false)) {
            CliOutput::error(out.find("error") != nullptr
                                 ? json::stringAt(out, "error", "analysis failed")
                                 : "analysis failed");
            return exitcode::kUsageError;
        }

        CliOutput::heading(std::cout, "Failure analysis");
        CliOutput::keyValue(std::cout, "test", json::stringAt(out, "test", ""), 22);
        const json::Value* verdict = out.find("verdict");
        CliOutput::keyValue(std::cout,
                            "engine verdict",
                            json::stringAt(*verdict, "status", "") + " / " +
                                json::stringAt(*verdict, "failure_category", ""),
                            22);
        std::cout << '\n';

        // Rendered from the same struct the JSON comes from, so the advisory
        // disclaimer cannot be dropped from one path and not the other.
        testforge::ai::AnalysisResult analysis;
        const json::Value* payload = out.find("analysis");
        analysis.ok = json::boolAt(*payload, "ok", false);
        analysis.probableCause = json::stringAt(*payload, "probable_cause", "");
        analysis.suggestedCategory = json::stringAt(*payload, "suggested_category", "");
        analysis.confidence = json::doubleAt(*payload, "confidence", 0.0);
        analysis.model = json::stringAt(*payload, "model", "");
        analysis.provider = json::stringAt(*payload, "provider", "");
        for (const json::Value& item : json::arrayAt(*payload, "evidence")) {
            analysis.evidence.push_back(item.stringOr(""));
        }
        for (const json::Value& item : json::arrayAt(*payload, "suggested_investigation")) {
            analysis.suggestedInvestigation.push_back(item.stringOr(""));
        }
        std::cout << analysis.render();
        return exitcode::kSuccess;
    }

    CliOutput::error("unknown ai subcommand '" + subcommand + "'",
                     "supported: status, generate-tests, analyze-failure");
    return exitcode::kUsageError;
}

// ---------------------------------------------------------------------------
// spec
// ---------------------------------------------------------------------------

int Commands::spec(api::TestForgeService& service, const ParsedCommand& parsed) {
    // Both spellings work: `spec <file>` and `spec load <file>`. The verb is
    // optional noise kept for the form the help has always advertised.
    std::string file = parsed.value("file");
    if (file.empty()) {
        std::size_t at = 0;
        if (at < parsed.positionals.size() &&
            strings::equalsIgnoreCase(parsed.positionals[at], "load")) {
            ++at;
        }
        if (at < parsed.positionals.size()) {
            file = parsed.positionals[at];
        }
    }
    if (file.empty()) {
        CliOutput::error("no specification file given",
                         "testforge spec path/to/spec.json --register --execute");
        return exitcode::kUsageError;
    }

    const bool wantsExecute = parsed.flag("execute");
    const json::Value out = service.loadSpecFile(file, parsed.flag("register") || wantsExecute);

    // --execute runs in this same process, because registration is in-memory:
    // a separate `testforge run` would find nothing to run.
    if (wantsExecute && out.find("ok") != nullptr && json::boolAt(out, "ok", false)) {
        RunOptions options;
        options.filter.suites = {out.find("suite") != nullptr
                                     ? json::stringAt(out, "suite", "generated")
                                     : std::string("generated")};
        options.workers = parsed.intValue("workers", 0);
        options.label = "spec:" + file;

        std::vector<std::shared_ptr<RunObserver>> observers;
        if (!wantsJson(parsed)) {
            reporting::ConsoleReporterOptions consoleOptions;
            consoleOptions.color = CliOutput::colorEnabled();
            observers.push_back(std::make_shared<reporting::ConsoleReporter>(consoleOptions));
        }

        const TestRun run = service.run(options, observers);
        if (wantsJson(parsed)) {
            json::Value combined = run.toJson();
            combined.set("ok", true);
            combined.set("specification", out);
            CliOutput::json(std::cout, combined);
        }
        return run.exitCode() == 0 ? exitcode::kSuccess : exitcode::kTestsFailed;
    }

    if (wantsJson(parsed)) {
        return emitJson(out);
    }

    const json::Value* validation = out.find("validation");
    const bool ok = json::boolAt(out, "ok", false);
    CliOutput::heading(std::cout, "Specification " + file);
    if (!ok) {
        CliOutput::error(out.find("error") != nullptr
                             ? json::stringAt(out, "error", "the specification was rejected")
                             : "the specification was rejected");
    }
    if (validation != nullptr) {
        CliOutput::keyValue(
            std::cout, "accepted", std::to_string(json::intAt(*validation, "accepted", 0)), 22);
        CliOutput::keyValue(
            std::cout, "rejected", std::to_string(json::intAt(*validation, "rejected", 0)), 22);
        for (const json::Value& issue : json::arrayAt(*validation, "issues")) {
            std::cout << "  "
                      << (json::boolAt(issue, "fatal", true) ? CliOutput::red("rejected")
                                                             : CliOutput::yellow("warn"))
                      << "  " << json::stringAt(issue, "where", "") << ": "
                      << json::stringAt(issue, "message", "") << '\n';
        }
    }
    if (out.find("registered") != nullptr) {
        CliOutput::keyValue(
            std::cout, "registered", std::to_string(json::intAt(out, "registered", 0)), 22);
    }
    return ok ? exitcode::kSuccess : exitcode::kUsageError;
}

// ---------------------------------------------------------------------------
// db
// ---------------------------------------------------------------------------

int Commands::database(api::TestForgeService& service, const ParsedCommand& parsed) {
    const ResultRepositoryPtr repository = service.repository();
    if (!repository) {
        CliOutput::error("no results database is open", "check database.path in the configuration");
        return exitcode::kUsageError;
    }

    const std::string subcommand =
        parsed.subcommand.empty() ? (parsed.flag("prune") ? "prune" : "info") : parsed.subcommand;

    if (subcommand == "prune") {
        const int days = parsed.intValue("days", service.config().database.retentionDays);
        const int removed = repository->pruneOlderThan(days);
        std::cout << "Removed " << removed << " run(s) older than " << days << " days.\n";
        return exitcode::kSuccess;
    }

    const json::Value info = repository->storageInfo();
    if (wantsJson(parsed)) {
        return emitJson(info);
    }
    CliOutput::heading(std::cout, "Results database");
    for (const json::Member& member : info.asObject()) {
        std::string text;
        if (member.second.isString()) {
            text = member.second.asString();
        } else if (member.second.isNumber()) {
            text = std::to_string(member.second.intOr(0));
        } else {
            text = member.second.dump();
        }
        CliOutput::keyValue(std::cout, member.first, text, 22);
    }
    return exitcode::kSuccess;
}

// ---------------------------------------------------------------------------
// version / help
// ---------------------------------------------------------------------------

int Commands::version(const ParsedCommand& parsed) {
    if (parsed.flag("json")) {
        json::Value out = json::Value::object();
        out.set("ok", true);
        out.set("version", Version::string());
        out.set("build", Version::banner());
        CliOutput::json(std::cout, out);
        return exitcode::kSuccess;
    }
    std::cout << Version::banner() << '\n';
    return exitcode::kSuccess;
}

int Commands::help(const std::string& command) {
    if (command == "run") {
        std::cout << R"(testforge run — execute tests

USAGE
  testforge run [selection] [options]

SELECTION
  --suite <glob>          run tests in matching suites (repeatable)
  --test <glob>           run tests whose name matches (repeatable)
  --tag <name>            run tests carrying the tag (repeatable)
  --exclude-tag <name>    skip tests carrying the tag
  --exclude-suite <glob>  skip matching suites
  --include-disabled      include tests marked disabled
  --shards N --shard I    run only shard I of N (stable, by test id)
  --shuffle [--seed N]    randomise order; the seed is reported so a
                          failing order can be reproduced exactly

OPTIONS
  --workers N             parallel workers (default: CPU count)
  --timeout MS            per-test deadline override
  --retry-failed N        re-run transient failures up to N times
  --fail-fast             stop scheduling after the first failure
  --label TEXT            label the run in history
  --no-persist            do not write to the results database
  --no-report             do not write report files
  --dry-run               list what would run, then exit
  --json                  emit the run as JSON instead of a report

EXIT CODES
  0  every test passed
  1  the run completed and some tests failed
  2  bad arguments or configuration
  3  TestForge itself failed

EXAMPLES
  testforge run
  testforge run --suite smoke --workers 8
  testforge run --tag api --tag regression --retry-failed 1
  testforge run api.health_check
)";
        return exitcode::kSuccess;
    }

    if (command == "ai") {
        std::cout << R"(testforge ai — AI-assisted testing (advisory only)

USAGE
  testforge ai status
  testforge ai generate-tests "<requirement>" [options]
  testforge ai analyze-failure [<run-id>] [--test NAME]

GENERATE-TESTS
  --requirement TEXT      the requirement, in plain language
  --file PATH             read the requirement from a file
  --suite NAME            suite name for the generated tests
  --max-tests N           cap on how many to generate
  --register              register them so they can be run
  --mock                  use the offline deterministic provider

ANALYZE-FAILURE
  --run ID                which run (default: the most recent)
  --test NAME             which test (default: the first failure)

NOTES
  Generated specifications are validated before anything runs: methods,
  endpoints and headers are checked against an allow-list, and a spec that
  fails validation is rejected rather than repaired.

  AI analysis never changes a verdict. Pass and fail are decided by the
  assertions in the C++ engine; the model only suggests where to look.

  A live model needs the Python sidecar and OPENAI_API_KEY:
    uvicorn python.ai.service:app --port 8810
)";
        return exitcode::kSuccess;
    }

    if (command == "serve") {
        std::cout << R"(testforge serve — REST API and dashboard

USAGE
  testforge serve [--host H] [--port P] [--workers N] [--static DIR]

The server binds to 127.0.0.1 by default and has no authentication.
POST /api/runs executes registered tests, so do not expose this port to an
untrusted network. See docs/security.md.
)";
        return exitcode::kSuccess;
    }

    std::cout << R"(TestForge — intelligent C++ test automation and validation

USAGE
  testforge <command> [options]

COMMANDS
  list        show registered tests
  run         execute tests
  report      render a report for a stored run
  diagnose    collect system and GPU diagnostics
  history     list previous runs, or one test's history
  stats       aggregate analytics across runs
  serve       start the REST API and dashboard
  ai          generate tests, or analyse a failure (advisory)
  spec        load and validate a test specification file
  db          inspect or prune the results database
  version     print the version
  help        show this message

GLOBAL OPTIONS  (these follow the command: `testforge run --no-db`)
  --config PATH       configuration file (JSON)
  --log-level LEVEL   trace | debug | info | warn | error
  --log-file PATH     also write JSON logs to a file
  --log-json          machine-readable logs on stderr
  --db PATH           results database path
  --no-db             disable persistence for this invocation
  --no-color          disable ANSI colour (also honours NO_COLOR)
  -h, --help          help, optionally for one command
  -V, --version       version

EXAMPLES
  testforge list --suite smoke
  testforge run --suite regression --workers 8
  testforge run --tag api --retry-failed 1
  testforge diagnose --gpu
  testforge history --limit 10
  testforge stats --flaky
  testforge ai generate-tests "Users must have a unique email" --mock --register
  testforge serve --port 8080

Run 'testforge help <command>' or 'testforge <command> --help' for detail.
)";
    return exitcode::kSuccess;
}

}  // namespace testforge::cli
