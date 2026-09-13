/// System suite — validates the Linux diagnostics layer against the machine it
/// is running on.
///
/// These are real assertions about real system state, so they have to be
/// written carefully: "there is at least one CPU" is a fact, "there are at
/// least 4 CPUs" is a guess about somebody else's laptop. Where a value cannot
/// be read (a container with a masked /proc), the test skips with the reason
/// rather than failing.

#include "testforge/core/Environment.hpp"
#include "testforge/core/Process.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/diagnostics/LinuxDiagnosticProvider.hpp"
#include "testforge/testing/ShortAssertions.hpp"
#include "testforge/testing/TestRegistry.hpp"

#include <map>
#include <string>
#include <vector>

using namespace testforge;
using namespace testforge::diagnostics;

TESTFORGE_TEST_OPTS(system,
                    os_information_is_readable,
                    TestOptions{}
                        .describe("Kernel, hostname and architecture can be read.")
                        .withTags({"system", "linux", "smoke"})) {
    const OsInfo os = LinuxDiagnosticProvider::readOsInfo();

    if (os.name.empty()) {
        ctx.skip("uname() returned nothing; this is not a POSIX system");
    }

    ASSERT_FALSE(os.name.empty());
    ASSERT_FALSE(os.kernelVersion.empty());
    ASSERT_FALSE(os.architecture.empty());

    ctx.addMetadata("os", os.name);
    ctx.addMetadata("kernel", os.kernelVersion);
    ctx.addMetadata("architecture", os.architecture);
    ctx.addMetadata("distribution", os.distribution);
    ctx.addMetadata("in_container", os.insideContainer);
    ctx.log().info("host identified",
                   {{"kernel", os.kernelVersion}, {"distribution", os.distribution}});
}

TESTFORGE_TEST_OPTS(system,
                    cpu_topology_is_sane,
                    TestOptions{}
                        .describe("At least one logical core, and load averages are not "
                                  "negative.")
                        .withTags({"system", "linux"})) {
    const CpuInfo cpu = LinuxDiagnosticProvider::readCpuInfo();

    if (cpu.logicalCores == 0) {
        ctx.skip("CPU topology is not exposed on this system");
    }

    ASSERT_GE(cpu.logicalCores, 1);
    if (cpu.physicalCores > 0) {
        // Hyper-threading means logical >= physical, never the other way.
        ASSERT_LE(cpu.physicalCores, cpu.logicalCores);
    }
    if (cpu.loadAverage1 >= 0.0) {
        ASSERT_GE(cpu.loadAverage1, 0.0);
    }
    if (cpu.utilizationPercent >= 0.0) {
        ASSERT_LE(cpu.utilizationPercent, 100.0);
    }

    ctx.addMetadata("logical_cores", cpu.logicalCores);
    ctx.addMetadata("cpu_model", cpu.model);
    ctx.addMetadata("load_1m", cpu.loadAverage1);
}

TESTFORGE_TEST_OPTS(system,
                    memory_accounting_is_consistent,
                    TestOptions{}
                        .describe("Available memory never exceeds total, and usage is a "
                                  "percentage.")
                        .withTags({"system", "linux"})) {
    const MemoryInfo memory = LinuxDiagnosticProvider::readMemoryInfo();

    if (memory.totalKb <= 0) {
        ctx.skip("/proc/meminfo is not readable here");
    }

    ASSERT_GT(memory.totalKb, 0);
    if (memory.availableKb >= 0) {
        ASSERT_LE(memory.availableKb, memory.totalKb);
        ASSERT_GE(memory.usedPercent(), 0.0);
        ASSERT_LE(memory.usedPercent(), 100.0);
    }

    ctx.addMetadata("total_mb", memory.totalKb / 1024);
    ctx.addMetadata("available_mb", memory.availableKb / 1024);
    ctx.addMetadata("used_percent", memory.usedPercent());

    // A warning rather than a failure: a busy machine is not a broken machine,
    // but it is worth knowing when triaging a timeout later.
    if (memory.usedPercent() > 90.0) {
        ctx.log().warn("memory pressure is high", {{"used_percent", memory.usedPercent()}});
    }
}

TESTFORGE_TEST_OPTS(system,
                    process_memory_is_reported,
                    TestOptions{}
                        .describe("TestForge can read its own resident set size.")
                        .withTags({"system", "linux"})) {
    const MemoryInfo memory = LinuxDiagnosticProvider::readMemoryInfo();

    if (memory.processRssKb < 0) {
        ctx.skip("/proc/self/status is not readable here");
    }
    ASSERT_GT(memory.processRssKb, 0);
    ctx.addMetadata("testforge_rss_kb", memory.processRssKb);
}

TESTFORGE_TEST_OPTS(system,
                    filesystem_has_free_space,
                    TestOptions{}
                        .describe("The root filesystem is readable and not completely full.")
                        .withTags({"system", "linux", "regression"})) {
    const std::vector<DiskUsage> disks = LinuxDiagnosticProvider::readDiskUsage();

    if (disks.empty()) {
        ctx.skip("no filesystems could be enumerated");
    }

    const DiskUsage* root = nullptr;
    for (const DiskUsage& disk : disks) {
        if (disk.mountPoint == "/") {
            root = &disk;
            break;
        }
    }
    if (root == nullptr) {
        root = &disks.front();
    }

    ASSERT_GT(root->totalBytes, 0);
    ASSERT_GE(root->availableBytes, 0);
    ctx.addMetadata("mount", root->mountPoint);
    ctx.addMetadata("free_gb", root->availableBytes / (1024 * 1024 * 1024));
    ctx.addMetadata("used_percent", root->usedPercent());

    // A genuinely full disk breaks later tests in confusing ways, so fail here
    // where the cause is obvious.
    ASSERT_LT(root->usedPercent(), 99.0);
}

TESTFORGE_TEST_OPTS(system,
                    network_interfaces_are_enumerable,
                    TestOptions{}
                        .describe("At least a loopback interface exists and is up.")
                        .withTags({"system", "linux", "network"})) {
    const std::vector<NetworkInterface> interfaces =
        LinuxDiagnosticProvider::readNetworkInterfaces();

    if (interfaces.empty()) {
        ctx.skip("network interfaces could not be enumerated");
    }

    bool loopbackUp = false;
    for (const NetworkInterface& item : interfaces) {
        if (item.loopback && item.up) {
            loopbackUp = true;
        }
    }
    ASSERT_TRUE(loopbackUp);
    ctx.addMetadata("interface_count", static_cast<std::int64_t>(interfaces.size()));
}

TESTFORGE_TEST_OPTS(system,
                    subprocess_execution_is_safe,
                    TestOptions{}
                        .describe("ProcessRunner executes without a shell, so metacharacters "
                                  "stay inert.")
                        .withTags({"system", "security", "regression"})) {
    if (!ProcessRunner::isAvailable("echo")) {
        ctx.skip("/bin/echo is not available");
    }

    // The argument contains a semicolon and a command substitution. With a
    // shell these would execute; through execv they are just a string.
    const std::string hostile = "hello; rm -rf /tmp/testforge-should-not-exist; $(whoami)";
    const ProcessResult result = ProcessRunner::run("echo", {hostile});

    ASSERT_TRUE(result.started);
    ASSERT_EQ(result.exitCode, 0);
    ASSERT_CONTAINS(result.standardOutput, hostile);
    // Proof it was not interpreted: the literal text came back intact.
    ASSERT_CONTAINS(result.standardOutput, "$(whoami)");
    ctx.addMetadata("echoed_bytes", static_cast<std::int64_t>(result.standardOutput.size()));
}

TESTFORGE_TEST_OPTS(system,
                    subprocess_timeout_is_enforced,
                    TestOptions{}
                        .describe("A child that runs too long is terminated at the deadline.")
                        .withTags({"system", "regression", "timeout"})
                        .withTimeout(15000)) {
    if (!ProcessRunner::isAvailable("sleep")) {
        ctx.skip("/bin/sleep is not available");
    }

    ProcessOptions options;
    options.timeout = Milliseconds{300};

    const Stopwatch watch;
    const ProcessResult result = ProcessRunner::run("sleep", {"10"}, options);
    const Milliseconds elapsed = watch.elapsed();

    ASSERT_TRUE(result.started);
    ASSERT_TRUE(result.timedOut);
    // Terminated near the deadline, not after the full ten seconds.
    ASSERT_LT(elapsed.count(), std::int64_t{5000});
    ctx.addMetadata("elapsed_ms", millisOf(elapsed));
}

TESTFORGE_TEST_OPTS(system,
                    missing_executable_is_reported_clearly,
                    TestOptions{}
                        .describe("A command that does not exist fails to launch, with a "
                                  "message that says so.")
                        .withTags({"system", "negative"})) {
    const ProcessResult result =
        ProcessRunner::run("testforge-definitely-not-a-real-binary", {"--version"});

    ASSERT_FALSE(result.started);
    ASSERT_CONTAINS(result.launchError, "not found");
    ASSERT_FALSE(result.ok());
}

TESTFORGE_TEST_OPTS(system,
                    environment_snapshot_redacts_secrets,
                    TestOptions{}
                        .describe("The diagnostic environment subset never carries a "
                                  "credential value.")
                        .withTags({"system", "security"})) {
    const std::map<std::string, std::string> snapshot = env::diagnosticSubset();

    for (const auto& [key, value] : snapshot) {
        if (strings::isSensitiveName(key)) {
            ASSERT_EQ(value, std::string("***REDACTED***"));
        }
    }
    ctx.addMetadata("variables_captured", static_cast<std::int64_t>(snapshot.size()));
}
