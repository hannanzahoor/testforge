/// Tests for the diagnostics layer.
///
/// The GPU tests here are where the mock provider belongs: the thing under
/// test is the parsing and the reporting contract, not the hardware. Real
/// hardware is exercised by the `gpu` suite in examples/, which skips when
/// there is no GPU. Nothing in this file claims to have measured a device.

#include "testforge/core/Process.hpp"
#include "testforge/diagnostics/DiagnosticProvider.hpp"
#include "testforge/diagnostics/GpuProviders.hpp"
#include "testforge/diagnostics/LinuxDiagnosticProvider.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace testforge;
using namespace testforge::diagnostics;

// ---------------------------------------------------------------------------
// Process execution — the security-critical piece
// ---------------------------------------------------------------------------

TEST(Process, RunsACommandAndCapturesOutput) {
    if (!ProcessRunner::isAvailable("echo")) {
        GTEST_SKIP() << "/bin/echo is not available";
    }
    const ProcessResult result = ProcessRunner::run("echo", {"hello", "world"});
    EXPECT_TRUE(result.started);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.standardOutput.find("hello world"), std::string::npos);
}

TEST(Process, ShellMetacharactersAreInertData) {
    if (!ProcessRunner::isAvailable("echo")) {
        GTEST_SKIP() << "/bin/echo is not available";
    }
    // execv, not system(): a semicolon is a character in an argument, not a
    // command separator. This is the test that would catch a regression to
    // shell execution.
    const std::string hostile = "a; touch /tmp/testforge-pwned; $(whoami) `id` && echo no";
    const ProcessResult result = ProcessRunner::run("echo", {hostile});

    EXPECT_TRUE(result.ok());
    EXPECT_NE(result.standardOutput.find(hostile), std::string::npos)
        << "the argument came back changed, which means something interpreted it";
    EXPECT_NE(result.standardOutput.find("$(whoami)"), std::string::npos);
    EXPECT_NE(result.standardOutput.find("`id`"), std::string::npos);
}

TEST(Process, NonZeroExitIsReportedNotThrown) {
    if (!ProcessRunner::isAvailable("false")) {
        GTEST_SKIP() << "/bin/false is not available";
    }
    const ProcessResult result = ProcessRunner::run("false", {});
    EXPECT_TRUE(result.started);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.exitCode, 0);
}

TEST(Process, MissingExecutableFailsToLaunch) {
    const ProcessResult result = ProcessRunner::run("testforge-no-such-binary", {"--version"});
    EXPECT_FALSE(result.started);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.launchError.find("not found"), std::string::npos);
}

TEST(Process, TimeoutTerminatesTheChild) {
    if (!ProcessRunner::isAvailable("sleep")) {
        GTEST_SKIP() << "/bin/sleep is not available";
    }
    ProcessOptions options;
    options.timeout = Milliseconds{200};

    const Stopwatch watch;
    const ProcessResult result = ProcessRunner::run("sleep", {"30"}, options);

    EXPECT_TRUE(result.started);
    EXPECT_TRUE(result.timedOut);
    EXPECT_LT(watch.elapsed().count(), 5000) << "the child outlived its deadline";
}

TEST(Process, StderrIsCapturedSeparately) {
    if (!ProcessRunner::isAvailable("sh")) {
        GTEST_SKIP() << "/bin/sh is not available";
    }
    // Running sh here is deliberate and safe: the script is a fixed literal in
    // this test, not user input. It is the only way to produce stderr from a
    // standard utility.
    const ProcessResult result = ProcessRunner::run("sh", {"-c", "echo out; echo err 1>&2"});
    EXPECT_TRUE(result.ok());
    EXPECT_NE(result.standardOutput.find("out"), std::string::npos);
    EXPECT_NE(result.standardError.find("err"), std::string::npos);
}

TEST(Process, WhichResolvesAgainstPath) {
    if (!ProcessRunner::isAvailable("echo")) {
        GTEST_SKIP() << "/bin/echo is not available";
    }
    EXPECT_FALSE(ProcessRunner::which("echo").empty());
    EXPECT_TRUE(ProcessRunner::which("testforge-no-such-binary").empty());
    // A name with a slash is a path and is never searched for on PATH.
    EXPECT_TRUE(ProcessRunner::which("./definitely-not-here").empty());
}

TEST(Process, ArgumentsWithNulOrNewlineAreRejected) {
    EXPECT_FALSE(ProcessRunner::isSafeArgument(std::string("a\0b", 3)));
    EXPECT_FALSE(ProcessRunner::isSafeArgument("a\nb"));
    EXPECT_TRUE(ProcessRunner::isSafeArgument("a;b|c$(d)"));
}

TEST(Process, ExtraEnvironmentReachesTheChild) {
    if (!ProcessRunner::isAvailable("sh")) {
        GTEST_SKIP() << "/bin/sh is not available";
    }
    ProcessOptions options;
    options.extraEnvironment = {{"TESTFORGE_CHILD_VALUE", "visible"}};
    const ProcessResult result =
        ProcessRunner::run("sh", {"-c", "echo $TESTFORGE_CHILD_VALUE"}, options);
    EXPECT_NE(result.standardOutput.find("visible"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Linux diagnostics
// ---------------------------------------------------------------------------

TEST(LinuxDiagnostics, IsAvailableOnThisPlatform) {
    const LinuxDiagnosticProvider provider;
#if defined(_WIN32)
    EXPECT_FALSE(provider.isAvailable());
#else
    EXPECT_TRUE(provider.isAvailable());
    EXPECT_EQ(provider.name(), "system");
#endif
}

TEST(LinuxDiagnostics, ReadsPlausibleSystemState) {
#if defined(_WIN32)
    GTEST_SKIP() << "POSIX only";
#else
    const OsInfo os = LinuxDiagnosticProvider::readOsInfo();
    EXPECT_FALSE(os.name.empty());
    EXPECT_FALSE(os.kernelVersion.empty());

    const CpuInfo cpu = LinuxDiagnosticProvider::readCpuInfo();
    EXPECT_GE(cpu.logicalCores, 1);

    const MemoryInfo memory = LinuxDiagnosticProvider::readMemoryInfo();
    if (memory.totalKb > 0) {
        EXPECT_LE(memory.availableKb, memory.totalKb);
        EXPECT_GE(memory.usedPercent(), 0.0);
        EXPECT_LE(memory.usedPercent(), 100.0);
    }
#endif
}

TEST(LinuxDiagnostics, SnapshotSerialisesToJson) {
    const LinuxDiagnosticProvider provider;
    DiagnosticsConfig config;
    config.includeProcesses = false;

    const json::Value document = provider.collect(config);
    EXPECT_TRUE(document.isObject());
    EXPECT_NO_THROW((void)json::parse(document.dump()));
}

TEST(LinuxDiagnostics, UnknownValuesAreAbsentRatherThanZero) {
    // "0 MB available" and "we could not read it" must not look the same.
    MemoryInfo memory;  // every field at its unknown default
    const json::Value document = memory.toJson();
    EXPECT_FALSE(document.contains("total_kb"));
    EXPECT_EQ(memory.usedPercent(), -1.0);
}

TEST(LinuxDiagnostics, CollectNeverThrows) {
    // Diagnostics run while a failure is already being handled; replacing the
    // real failure with a diagnostics failure would be actively unhelpful.
    const LinuxDiagnosticProvider provider;
    DiagnosticsConfig config;
    config.includeProcesses = true;
    config.includeSystemLogs = true;
    config.commandTimeoutMs = 1000;
    EXPECT_NO_THROW((void)provider.collect(config));
}

// ---------------------------------------------------------------------------
// GPU providers
// ---------------------------------------------------------------------------

TEST(NvidiaGpu, ParsesRealisticQueryOutput) {
    // Captured from `nvidia-smi --query-gpu=... --format=csv,noheader,nounits`.
    // Testing the parser against fixed text is what lets this run on a machine
    // with no GPU.
    const std::string csv =
        "0, NVIDIA A100-SXM4-40GB, GPU-abc123, 550.54.14, 40960, 1024, 39936, 37, 3, 42, 68.55\n"
        "1, NVIDIA A100-SXM4-40GB, GPU-def456, 550.54.14, 40960, 20480, 20480, 95, 50, 71, "
        "250.10\n";

    const std::vector<GpuDevice> devices = NvidiaGpuProvider::parseQueryCsv(csv);
    ASSERT_EQ(devices.size(), 2U);

    EXPECT_EQ(devices[0].index, 0);
    EXPECT_EQ(devices[0].name, "NVIDIA A100-SXM4-40GB");
    EXPECT_EQ(devices[0].driverVersion, "550.54.14");
    EXPECT_EQ(devices[0].memoryTotalMb, 40960);
    EXPECT_EQ(devices[0].memoryUsedMb, 1024);
    EXPECT_DOUBLE_EQ(devices[0].utilizationPercent, 37.0);
    EXPECT_DOUBLE_EQ(devices[0].temperatureCelsius, 42.0);
    EXPECT_DOUBLE_EQ(devices[0].powerDrawWatts, 68.55);

    EXPECT_EQ(devices[1].index, 1);
    EXPECT_EQ(devices[1].memoryUsedMb, 20480);
}

TEST(NvidiaGpu, NotApplicableFieldsStayUnknown) {
    // Cards that do not report a sensor print [N/A]; that must become "unknown",
    // never zero, or a report would claim the GPU is at 0 degrees.
    const std::string csv =
        "0, NVIDIA T400, GPU-xyz, 535.104.05, 4096, 512, 3584, [N/A], [N/A], [N/A], [Not "
        "Supported]\n";

    const std::vector<GpuDevice> devices = NvidiaGpuProvider::parseQueryCsv(csv);
    ASSERT_EQ(devices.size(), 1U);
    EXPECT_EQ(devices[0].memoryTotalMb, 4096);
    EXPECT_LT(devices[0].utilizationPercent, 0.0);
    EXPECT_LT(devices[0].temperatureCelsius, 0.0);
    EXPECT_LT(devices[0].powerDrawWatts, 0.0);
}

TEST(NvidiaGpu, MalformedRowsAreSkippedNotGuessedAt) {
    const std::string csv =
        "0, NVIDIA A100, GPU-abc, 550.54.14, 40960, 1024, 39936, 37, 3, 42, 68.55\n"
        "this line is not csv\n"
        "1, too, few, fields\n";
    EXPECT_EQ(NvidiaGpuProvider::parseQueryCsv(csv).size(), 1U);
}

TEST(NvidiaGpu, ParsesComputeApps) {
    const std::string csv = "1234, python3, 512\n5678, ./train, 2048\n";
    const std::vector<GpuProcessInfo> processes = NvidiaGpuProvider::parseComputeAppsCsv(csv);
    ASSERT_EQ(processes.size(), 2U);
    EXPECT_EQ(processes[0].pid, 1234);
    EXPECT_EQ(processes[0].name, "python3");
    EXPECT_EQ(processes[1].usedMemoryMb, 2048);
}

TEST(NvidiaGpu, QueryArgumentsAreFixed) {
    // Nothing user-supplied may ever reach this command line.
    const std::vector<std::string> arguments = NvidiaGpuProvider::queryArguments();
    ASSERT_EQ(arguments.size(), 2U);
    EXPECT_NE(arguments[0].find("--query-gpu="), std::string::npos);
    EXPECT_EQ(arguments[1], "--format=csv,noheader,nounits");
}

TEST(NvidiaGpu, ReportsAbsenceHonestly) {
    const NvidiaGpuProvider provider;
    const GpuSnapshot snapshot = provider.query(DiagnosticsConfig{});

    // The invariant that matters on every machine: real provider, never mock.
    EXPECT_FALSE(snapshot.isMockData);

    if (snapshot.available) {
        // A machine with a GPU: values must be present and coherent.
        EXPECT_FALSE(snapshot.devices.empty());
        EXPECT_TRUE(snapshot.unavailableReason.empty());
    } else {
        // A machine without one: a reason, and no invented devices.
        EXPECT_TRUE(snapshot.devices.empty());
        EXPECT_FALSE(snapshot.unavailableReason.empty());
        EXPECT_NE(snapshot.summary().find("unavailable"), std::string::npos);
    }
}

TEST(MockGpu, IsAlwaysLabelledAsMock) {
    const MockGpuProvider provider(2, "MOCK-GPU");
    const GpuSnapshot snapshot = provider.query(DiagnosticsConfig{});

    ASSERT_TRUE(snapshot.available);
    EXPECT_TRUE(snapshot.isMockData);
    EXPECT_EQ(snapshot.source, "mock");
    EXPECT_EQ(snapshot.devices.size(), 2U);
    // The flag must survive serialisation, so no consumer can miss it.
    EXPECT_TRUE(snapshot.toJson().at("is_mock_data").asBool());
    EXPECT_NE(snapshot.summary().find("MOCK TEST DATA"), std::string::npos);
}

TEST(MockGpu, CanSimulateAbsence) {
    MockGpuProvider provider;
    provider.setUnavailable("simulated: no driver");
    const GpuSnapshot snapshot = provider.query(DiagnosticsConfig{});

    EXPECT_FALSE(snapshot.available);
    EXPECT_TRUE(snapshot.isMockData);
    EXPECT_EQ(snapshot.unavailableReason, "simulated: no driver");
}

TEST(NullGpu, ExplainsWhyThereIsNothing) {
    const NullGpuProvider provider("no GPU here");
    const GpuSnapshot snapshot = provider.query(DiagnosticsConfig{});
    EXPECT_FALSE(snapshot.available);
    EXPECT_FALSE(snapshot.isMockData);
    EXPECT_EQ(snapshot.source, "none");
    EXPECT_EQ(snapshot.unavailableReason, "no GPU here");
}

TEST(DefaultGpuProvider, IsNeverTheMock) {
    // Synthetic values must only ever be installed deliberately by a test.
    const auto provider = createDefaultGpuProvider();
    ASSERT_NE(provider, nullptr);
    EXPECT_FALSE(provider->query(DiagnosticsConfig{}).isMockData);
}

// ---------------------------------------------------------------------------
// Collector
// ---------------------------------------------------------------------------

TEST(DiagnosticCollector, CollectsFromEveryRegisteredProvider) {
    DiagnosticsConfig config;
    config.includeGpu = true;
    const auto collector = DiagnosticCollector::createDefault(config);

    const json::Value document = collector->collectAll();
    EXPECT_TRUE(document.contains("collected_at"));
    EXPECT_TRUE(document.contains("system"));
    EXPECT_TRUE(document.contains("gpu"));
}

TEST(DiagnosticCollector, MockProviderCanBeInjected) {
    DiagnosticsConfig config;
    const auto collector = DiagnosticCollector::createDefault(config);
    collector->setGpuProvider(std::make_shared<MockGpuProvider>(1));

    const GpuSnapshot snapshot = collector->gpu();
    EXPECT_TRUE(snapshot.isMockData);

    // Replacing must not leave the previous provider registered as well.
    const json::Value document = collector->collectAll();
    EXPECT_TRUE(document.path("gpu.is_mock_data")->asBool());
}

TEST(DiagnosticCollector, FailurePayloadIsSmallerThanTheFullOne) {
    DiagnosticsConfig config;
    config.includeProcesses = true;
    const auto collector = DiagnosticCollector::createDefault(config);

    const std::size_t full = collector->collectAll().dump().size();
    const std::size_t forFailure = collector->collectForFailure().dump().size();

    // The failure payload is stored next to every failing result, so it is
    // deliberately trimmed.
    EXPECT_GT(full, 0U);
    EXPECT_GT(forFailure, 0U);
    EXPECT_LE(forFailure, full);
}

TEST(DiagnosticCollector, GpuCanBeDisabled) {
    DiagnosticsConfig config;
    config.includeGpu = false;
    const auto collector = DiagnosticCollector::createDefault(config);

    EXPECT_FALSE(collector->gpu().available);
    EXPECT_FALSE(collector->collectForFailure().contains("gpu"));
}

TEST(DiagnosticCollector, NeverThrows) {
    DiagnosticsConfig config;
    config.includeSystemLogs = true;
    config.includeProcesses = true;
    const auto collector = DiagnosticCollector::createDefault(config);
    EXPECT_NO_THROW((void)collector->collectAll());
    EXPECT_NO_THROW((void)collector->collectForFailure());
}
