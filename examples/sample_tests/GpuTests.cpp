/// GPU suite — validates NVIDIA diagnostics when hardware is present.
///
/// The governing rule: on a machine with no NVIDIA GPU these tests SKIP. They
/// do not fail, and they never substitute mock values for measurements. A
/// skipped GPU test on a laptop is the correct result; a passing one would be
/// a lie.
///
/// The mock provider is exercised separately, in the framework's own unit
/// tests (tests/unit/GpuProviderTest.cpp), where synthetic data is
/// appropriate because the thing under test is the parser, not the hardware.

#include "testforge/core/Process.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/diagnostics/GpuProviders.hpp"
#include "testforge/testing/ShortAssertions.hpp"
#include "testforge/testing/TestRegistry.hpp"

#include <string>
#include <vector>

using namespace testforge;
using namespace testforge::diagnostics;

namespace {

/// Queries real hardware, or skips with the reason it could not.
GpuSnapshot requireGpu(TestContext& ctx) {
    const NvidiaGpuProvider provider;
    if (!provider.isAvailable()) {
        ctx.skip("no NVIDIA GPU detected (nvidia-smi is not on PATH)");
    }

    const GpuSnapshot snapshot = provider.query(ctx.config().diagnostics);
    if (!snapshot.available) {
        ctx.skip("GPU diagnostics unavailable: " + snapshot.unavailableReason);
    }
    return snapshot;
}

}  // namespace

TESTFORGE_TEST_OPTS(gpu,
                    driver_is_present,
                    TestOptions{}
                        .describe("nvidia-smi is installed and reports a driver version.")
                        .withTags({"gpu", "hardware"})
                        .withTimeout(20000)) {
    const GpuSnapshot snapshot = requireGpu(ctx);

    ASSERT_TRUE(snapshot.available);
    // Belt and braces: a test asserting on hardware must never be looking at
    // synthetic values.
    ASSERT_FALSE(snapshot.isMockData);
    ASSERT_EQ(snapshot.source, std::string("nvidia-smi"));
    ASSERT_FALSE(snapshot.driverVersion.empty());

    ctx.addMetadata("driver_version", snapshot.driverVersion);
    ctx.addMetadata("cuda_version", snapshot.cudaVersion);
    ctx.addMetadata("device_count", static_cast<std::int64_t>(snapshot.devices.size()));
    ctx.log().info("GPU detected", {{"summary", snapshot.summary()}});
}

TESTFORGE_TEST_OPTS(gpu,
                    devices_are_enumerated,
                    TestOptions{}
                        .describe("Every reported device has a name and an index.")
                        .withTags({"gpu", "hardware"})
                        .withTimeout(20000)) {
    const GpuSnapshot snapshot = requireGpu(ctx);

    ASSERT_GE(snapshot.devices.size(), std::size_t{1});
    for (const GpuDevice& device : snapshot.devices) {
        ASSERT_FALSE(device.name.empty());
        ASSERT_GE(device.index, 0);
    }
    ctx.addMetadata("first_device", snapshot.devices.front().name);
}

TESTFORGE_TEST_OPTS(gpu,
                    memory_is_visible_and_consistent,
                    TestOptions{}
                        .describe("Used memory never exceeds total, and free is the remainder.")
                        .withTags({"gpu", "hardware", "regression"})
                        .withTimeout(20000)) {
    const GpuSnapshot snapshot = requireGpu(ctx);

    for (const GpuDevice& device : snapshot.devices) {
        if (device.memoryTotalMb < 0) {
            // Some virtualised GPUs do not expose memory; that is not a fault.
            ctx.log().warn("device does not report memory", {{"device", device.name}});
            continue;
        }
        ASSERT_GT(device.memoryTotalMb, 0);
        if (device.memoryUsedMb >= 0) {
            ASSERT_LE(device.memoryUsedMb, device.memoryTotalMb);
        }
        if (device.memoryFreeMb >= 0 && device.memoryUsedMb >= 0) {
            // nvidia-smi rounds, so allow a small discrepancy rather than
            // demanding exact arithmetic.
            const std::int64_t sum = device.memoryUsedMb + device.memoryFreeMb;
            ASSERT_NEAR_TO(static_cast<double>(sum),
                           static_cast<double>(device.memoryTotalMb),
                           static_cast<double>(device.memoryTotalMb) * 0.05);
        }
        ctx.addMetadata("gpu_memory_total_mb", device.memoryTotalMb);
        ctx.addMetadata("gpu_memory_used_mb", device.memoryUsedMb);
    }
}

TESTFORGE_TEST_OPTS(gpu,
                    utilization_is_within_range,
                    TestOptions{}
                        .describe("Utilisation and temperature are physically plausible.")
                        .withTags({"gpu", "hardware"})
                        .withTimeout(20000)) {
    const GpuSnapshot snapshot = requireGpu(ctx);

    for (const GpuDevice& device : snapshot.devices) {
        if (device.utilizationPercent >= 0.0) {
            ASSERT_GE(device.utilizationPercent, 0.0);
            ASSERT_LE(device.utilizationPercent, 100.0);
        }
        if (device.temperatureCelsius >= 0.0) {
            // A GPU reading over 130C is a broken sensor, not a hot GPU.
            ASSERT_LT(device.temperatureCelsius, 130.0);
        }
        if (device.powerDrawWatts >= 0.0) {
            ASSERT_LT(device.powerDrawWatts, 2000.0);
        }
    }
}

TESTFORGE_TEST_OPTS(gpu,
                    diagnostic_command_is_healthy,
                    TestOptions{}
                        .describe("nvidia-smi responds promptly and with the expected shape.")
                        .withTags({"gpu", "hardware", "regression"})
                        .withTimeout(20000)) {
    if (!ProcessRunner::isAvailable("nvidia-smi")) {
        ctx.skip("nvidia-smi is not installed");
    }

    ProcessOptions options;
    options.timeout = Milliseconds{5000};

    const Stopwatch watch;
    const ProcessResult result =
        ProcessRunner::run("nvidia-smi", NvidiaGpuProvider::queryArguments(), options);
    const Milliseconds elapsed = watch.elapsed();

    ASSERT_TRUE(result.started);
    ASSERT_FALSE(result.timedOut);
    if (result.exitCode != 0) {
        // The classic cause is a driver/library version mismatch after an
        // upgrade without a reboot. Surface it verbatim.
        ctx.skip("nvidia-smi exited " + std::to_string(result.exitCode) + ": " +
                 strings::truncate(result.standardError, 200));
    }
    ASSERT_EQ(result.exitCode, 0);
    ASSERT_FALSE(result.standardOutput.empty());
    ASSERT_LT(elapsed.count(), std::int64_t{5000});

    ctx.addMetadata("nvidia_smi_ms", millisOf(elapsed));
}

TESTFORGE_TEST_OPTS(gpu,
                    absence_is_reported_honestly,
                    TestOptions{}
                        .describe("With no GPU, the provider says so instead of inventing "
                                  "values.")
                        .withTags({"gpu", "contract"})) {
    // This one runs everywhere: it checks the *contract*, which is that the
    // no-hardware path produces an explicit, non-mock, unavailable result.
    const NvidiaGpuProvider provider;
    const GpuSnapshot snapshot = provider.query(ctx.config().diagnostics);

    ASSERT_FALSE(snapshot.isMockData);

    if (snapshot.available) {
        ASSERT_TRUE(snapshot.unavailableReason.empty());
        ASSERT_FALSE(snapshot.devices.empty());
        ctx.addMetadata("gpu_present", true);
    } else {
        ASSERT_FALSE(snapshot.unavailableReason.empty());
        ASSERT_TRUE(snapshot.devices.empty());
        ASSERT_CONTAINS(snapshot.summary(), "unavailable");
        ctx.addMetadata("gpu_present", false);
        ctx.addMetadata("reason", snapshot.unavailableReason);
    }
}
