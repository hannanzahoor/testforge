#include "testforge/diagnostics/GpuProviders.hpp"

#include "testforge/core/Logger.hpp"
#include "testforge/core/Process.hpp"
#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <sstream>

namespace testforge::diagnostics {
namespace {

/// The exact fields requested from nvidia-smi, in order. Kept as one list so
/// the request and the parser can never drift apart.
constexpr std::string_view kQueryFields =
    "index,name,uuid,driver_version,memory.total,memory.used,memory.free,"
    "utilization.gpu,utilization.memory,temperature.gpu,power.draw";

constexpr int kQueryFieldCount = 11;

/// nvidia-smi prints "[N/A]" or "[Not Supported]" for values a particular card
/// does not expose. Those must stay unknown, not become zero.
bool isNotApplicable(std::string_view text) {
    const std::string trimmed = strings::toLower(strings::trim(text));
    return trimmed.empty() || trimmed == "n/a" || trimmed == "[n/a]" ||
           trimmed == "[not supported]" || trimmed == "not supported" ||
           trimmed == "[unknown error]";
}

std::int64_t parseIntField(std::string_view text) {
    if (isNotApplicable(text)) {
        return -1;
    }
    // Values arrive as "16376 MiB" or bare numbers depending on the flag used.
    std::string digits;
    for (const char c : text) {
        if ((c >= '0' && c <= '9') || (digits.empty() && c == '-')) {
            digits.push_back(c);
        } else if (!digits.empty()) {
            break;
        }
    }
    std::int64_t value = -1;
    if (!digits.empty() && strings::parseInt(digits, value)) {
        return value;
    }
    return -1;
}

double parseDoubleField(std::string_view text) {
    if (isNotApplicable(text)) {
        return -1.0;
    }
    std::string number;
    for (const char c : text) {
        if ((c >= '0' && c <= '9') || c == '.' || (number.empty() && c == '-')) {
            number.push_back(c);
        } else if (!number.empty()) {
            break;
        }
    }
    double value = -1.0;
    if (!number.empty() && strings::parseDouble(number, value)) {
        return value;
    }
    return -1.0;
}

}  // namespace

// ---------------------------------------------------------------------------
// NvidiaGpuProvider
// ---------------------------------------------------------------------------

std::vector<std::string> NvidiaGpuProvider::queryArguments() {
    return {std::string("--query-gpu=") + std::string(kQueryFields),
            "--format=csv,noheader,nounits"};
}

bool NvidiaGpuProvider::isAvailable() const {
    return ProcessRunner::isAvailable("nvidia-smi");
}

std::vector<GpuDevice> NvidiaGpuProvider::parseQueryCsv(const std::string& csv) {
    std::vector<GpuDevice> devices;
    for (const std::string& line : strings::splitLines(csv)) {
        const std::string trimmed = strings::trim(line);
        if (trimmed.empty()) {
            continue;
        }
        const std::vector<std::string> fields = strings::split(trimmed, ',', false);
        if (fields.size() < static_cast<std::size_t>(kQueryFieldCount)) {
            continue;  // unexpected shape; skip rather than guess
        }

        GpuDevice device;
        device.index = static_cast<int>(std::max<std::int64_t>(0, parseIntField(fields[0])));
        device.name = strings::trim(fields[1]);
        device.uuid = strings::trim(fields[2]);
        device.driverVersion = strings::trim(fields[3]);
        device.memoryTotalMb = parseIntField(fields[4]);
        device.memoryUsedMb = parseIntField(fields[5]);
        device.memoryFreeMb = parseIntField(fields[6]);
        device.utilizationPercent = parseDoubleField(fields[7]);
        device.memoryUtilizationPercent = parseDoubleField(fields[8]);
        device.temperatureCelsius = parseDoubleField(fields[9]);
        device.powerDrawWatts = parseDoubleField(fields[10]);
        devices.push_back(std::move(device));
    }
    return devices;
}

std::vector<GpuProcessInfo> NvidiaGpuProvider::parseComputeAppsCsv(const std::string& csv) {
    std::vector<GpuProcessInfo> processes;
    for (const std::string& line : strings::splitLines(csv)) {
        const std::string trimmed = strings::trim(line);
        if (trimmed.empty()) {
            continue;
        }
        const std::vector<std::string> fields = strings::split(trimmed, ',', false);
        if (fields.size() < 3) {
            continue;
        }
        GpuProcessInfo process;
        process.pid = static_cast<int>(std::max<std::int64_t>(0, parseIntField(fields[0])));
        process.name = strings::trim(fields[1]);
        process.usedMemoryMb = parseIntField(fields[2]);
        processes.push_back(std::move(process));
    }
    return processes;
}

GpuSnapshot NvidiaGpuProvider::query(const DiagnosticsConfig& config) const {
    GpuSnapshot snapshot;
    snapshot.source = "nvidia-smi";
    snapshot.isMockData = false;

    if (!isAvailable()) {
        snapshot.available = false;
        snapshot.unavailableReason =
            "nvidia-smi was not found on PATH, so no NVIDIA driver is installed on this machine.";
        return snapshot;
    }

    ProcessOptions options;
    options.timeout = Milliseconds{config.commandTimeoutMs};
    options.maxOutputBytes = 256u * 1024u;

    // Fixed executable, fixed argument vector, no shell. Nothing user-supplied
    // reaches this call — see docs/security.md.
    const ProcessResult result = ProcessRunner::run("nvidia-smi", queryArguments(), options);

    if (!result.started) {
        snapshot.available = false;
        snapshot.unavailableReason = "could not launch nvidia-smi: " + result.launchError;
        return snapshot;
    }
    if (result.timedOut) {
        snapshot.available = false;
        snapshot.unavailableReason = "nvidia-smi did not respond within " +
                                     std::to_string(config.commandTimeoutMs) +
                                     "ms; the driver may be wedged";
        return snapshot;
    }
    if (result.exitCode != 0) {
        // The usual cause is a driver/library version mismatch, and nvidia-smi
        // says so on stderr — pass that through rather than paraphrasing.
        snapshot.available = false;
        snapshot.unavailableReason =
            "nvidia-smi exited with code " + std::to_string(result.exitCode) + ": " +
            strings::truncate(strings::trim(result.standardError.empty() ? result.standardOutput
                                                                         : result.standardError),
                              300);
        return snapshot;
    }

    snapshot.devices = parseQueryCsv(result.standardOutput);
    if (snapshot.devices.empty()) {
        snapshot.available = false;
        snapshot.unavailableReason =
            "nvidia-smi ran successfully but reported no devices (no NVIDIA GPU is visible to "
            "this process; under WSL or in a container the device may not be passed through)";
        return snapshot;
    }

    snapshot.available = true;
    snapshot.driverVersion = snapshot.devices.front().driverVersion;

    // The CUDA version is only in the plain-text header, not in the CSV query.
    const ProcessResult version = ProcessRunner::run("nvidia-smi", {"--version"}, options);
    if (version.ok()) {
        for (const std::string& line : strings::splitLines(version.standardOutput)) {
            if (strings::containsIgnoreCase(line, "CUDA Version")) {
                const std::size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    snapshot.cudaVersion = strings::trim(line.substr(colon + 1));
                }
                break;
            }
        }
    }
    for (GpuDevice& device : snapshot.devices) {
        device.cudaVersion = snapshot.cudaVersion;
    }

    // Per-device process lists, best effort: this query is unsupported on some
    // consumer cards and inside containers.
    const ProcessResult apps = ProcessRunner::run(
        "nvidia-smi",
        {"--query-compute-apps=pid,process_name,used_memory", "--format=csv,noheader,nounits"},
        options);
    if (apps.ok()) {
        std::vector<GpuProcessInfo> processes = parseComputeAppsCsv(apps.standardOutput);
        if (!snapshot.devices.empty()) {
            // The query does not report which device each process is on, so
            // they are attached to the first device rather than guessed at.
            snapshot.devices.front().processes = std::move(processes);
        }
    }

    return snapshot;
}

json::Value NvidiaGpuProvider::collect(const DiagnosticsConfig& config) const noexcept {
    try {
        return query(config).toJson();
    } catch (const std::exception& error) {
        json::Value out = json::Value::object();
        out.set("available", false);
        out.set("is_mock_data", false);
        out.set("source", "nvidia-smi");
        out.set("unavailable_reason",
                std::string("GPU diagnostics threw an exception: ") + error.what());
        return out;
    } catch (...) {
        json::Value out = json::Value::object();
        out.set("available", false);
        out.set("is_mock_data", false);
        out.set("source", "nvidia-smi");
        out.set("unavailable_reason", "GPU diagnostics threw an unknown exception");
        return out;
    }
}

// ---------------------------------------------------------------------------
// MockGpuProvider
// ---------------------------------------------------------------------------

MockGpuProvider::MockGpuProvider(int deviceCount, std::string deviceName)
    : deviceCount_(deviceCount), deviceName_(std::move(deviceName)) {}

void MockGpuProvider::setUnavailable(std::string reason) {
    unavailableReason_ = std::move(reason);
}

GpuSnapshot MockGpuProvider::query(const DiagnosticsConfig& /*config*/) const {
    GpuSnapshot snapshot;
    // Both flags are set on every path: nothing this class produces may ever
    // be mistaken for a measurement from real hardware.
    snapshot.source = "mock";
    snapshot.isMockData = true;

    if (!unavailableReason_.empty()) {
        snapshot.available = false;
        snapshot.unavailableReason = unavailableReason_;
        return snapshot;
    }

    snapshot.available = deviceCount_ > 0;
    if (!snapshot.available) {
        snapshot.unavailableReason = "mock provider configured with zero devices";
        return snapshot;
    }

    snapshot.driverVersion = "000.00.00-mock";
    snapshot.cudaVersion = "0.0-mock";
    for (int i = 0; i < deviceCount_; ++i) {
        GpuDevice device;
        device.index = i;
        device.name = deviceName_ + (deviceCount_ > 1 ? "-" + std::to_string(i) : "");
        device.uuid = "GPU-MOCK-0000-0000-0000-00000000000" + std::to_string(i);
        device.driverVersion = snapshot.driverVersion;
        device.cudaVersion = snapshot.cudaVersion;
        device.memoryTotalMb = 16384;
        device.memoryUsedMb = 1024;
        device.memoryFreeMb = 15360;
        device.utilizationPercent = 12.0;
        device.memoryUtilizationPercent = 6.25;
        device.temperatureCelsius = 42.0;
        device.powerDrawWatts = 55.5;
        snapshot.devices.push_back(std::move(device));
    }
    return snapshot;
}

json::Value MockGpuProvider::collect(const DiagnosticsConfig& config) const noexcept {
    try {
        return query(config).toJson();
    } catch (...) {
        json::Value out = json::Value::object();
        out.set("available", false);
        out.set("is_mock_data", true);
        out.set("source", "mock");
        out.set("unavailable_reason", "mock provider threw");
        return out;
    }
}

// ---------------------------------------------------------------------------
// NullGpuProvider
// ---------------------------------------------------------------------------

NullGpuProvider::NullGpuProvider(std::string reason) : reason_(std::move(reason)) {}

GpuSnapshot NullGpuProvider::query(const DiagnosticsConfig& /*config*/) const {
    GpuSnapshot snapshot;
    snapshot.available = false;
    snapshot.isMockData = false;
    snapshot.source = "none";
    snapshot.unavailableReason = reason_;
    return snapshot;
}

json::Value NullGpuProvider::collect(const DiagnosticsConfig& config) const noexcept {
    return query(config).toJson();
}

std::shared_ptr<GpuDiagnosticProvider> createDefaultGpuProvider() {
    if (ProcessRunner::isAvailable("nvidia-smi")) {
        return std::make_shared<NvidiaGpuProvider>();
    }
    return std::make_shared<NullGpuProvider>(
        "No NVIDIA GPU detected (nvidia-smi is not installed on this machine).");
}

}  // namespace testforge::diagnostics
