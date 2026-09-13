#include "testforge/diagnostics/SystemInfo.hpp"

#include <sstream>

namespace testforge::diagnostics {
namespace {

/// Adds a field only when it holds a real measurement.
///
/// Absent is meaningfully different from zero: "0 MB of GPU memory" and "we
/// could not read GPU memory" must not look the same in a report.
void setIfKnown(json::Value& out, std::string key, std::int64_t value) {
    if (value >= 0) {
        out.set(std::move(key), value);
    }
}

void setIfKnown(json::Value& out, std::string key, double value) {
    if (value >= 0.0) {
        out.set(std::move(key), value);
    }
}

void setIfNotEmpty(json::Value& out, std::string key, const std::string& value) {
    if (!value.empty()) {
        out.set(std::move(key), value);
    }
}

}  // namespace

json::Value OsInfo::toJson() const {
    json::Value out = json::Value::object();
    setIfNotEmpty(out, "name", name);
    setIfNotEmpty(out, "distribution", distribution);
    setIfNotEmpty(out, "kernel_version", kernelVersion);
    setIfNotEmpty(out, "kernel_build", kernelBuild);
    setIfNotEmpty(out, "hostname", hostname);
    setIfNotEmpty(out, "architecture", architecture);
    setIfKnown(out, "uptime_seconds", uptimeSeconds);
    out.set("inside_container", insideContainer);
    setIfNotEmpty(out, "container_hint", containerHint);
    return out;
}

json::Value CpuInfo::toJson() const {
    json::Value out = json::Value::object();
    setIfNotEmpty(out, "model", model);
    if (physicalCores > 0) {
        out.set("physical_cores", physicalCores);
    }
    if (logicalCores > 0) {
        out.set("logical_cores", logicalCores);
    }
    setIfKnown(out, "mhz", mhz);
    setIfKnown(out, "load_average_1m", loadAverage1);
    setIfKnown(out, "load_average_5m", loadAverage5);
    setIfKnown(out, "load_average_15m", loadAverage15);
    setIfKnown(out, "utilization_percent", utilizationPercent);
    return out;
}

double MemoryInfo::usedPercent() const {
    if (totalKb <= 0 || availableKb < 0) {
        return -1.0;
    }
    const double used = static_cast<double>(totalKb - availableKb);
    return 100.0 * used / static_cast<double>(totalKb);
}

json::Value MemoryInfo::toJson() const {
    json::Value out = json::Value::object();
    setIfKnown(out, "total_kb", totalKb);
    setIfKnown(out, "available_kb", availableKb);
    setIfKnown(out, "free_kb", freeKb);
    setIfKnown(out, "buffers_kb", buffersKb);
    setIfKnown(out, "cached_kb", cachedKb);
    setIfKnown(out, "swap_total_kb", swapTotalKb);
    setIfKnown(out, "swap_free_kb", swapFreeKb);
    setIfKnown(out, "process_rss_kb", processRssKb);
    setIfKnown(out, "used_percent", usedPercent());
    return out;
}

double DiskUsage::usedPercent() const {
    if (totalBytes <= 0 || availableBytes < 0) {
        return -1.0;
    }
    const double used = static_cast<double>(totalBytes - availableBytes);
    return 100.0 * used / static_cast<double>(totalBytes);
}

json::Value DiskUsage::toJson() const {
    json::Value out = json::Value::object();
    out.set("mount_point", mountPoint);
    setIfNotEmpty(out, "filesystem", filesystem);
    setIfKnown(out, "total_bytes", totalBytes);
    setIfKnown(out, "available_bytes", availableBytes);
    setIfKnown(out, "used_percent", usedPercent());
    return out;
}

json::Value NetworkInterface::toJson() const {
    json::Value out = json::Value::object();
    out.set("name", name);
    out.set("up", up);
    out.set("loopback", loopback);
    json::Value list = json::Value::array();
    for (const std::string& address : addresses) {
        list.push(address);
    }
    out.set("addresses", list);
    setIfKnown(out, "rx_bytes", rxBytes);
    setIfKnown(out, "tx_bytes", txBytes);
    return out;
}

json::Value ProcessInfo::toJson() const {
    json::Value out = json::Value::object();
    out.set("pid", pid);
    out.set("name", name);
    setIfNotEmpty(out, "state", state);
    setIfKnown(out, "rss_kb", rssKb);
    setIfKnown(out, "cpu_time_seconds", cpuTimeSeconds);
    return out;
}

json::Value SystemSnapshot::toJson() const {
    json::Value out = json::Value::object();
    out.set("os", os.toJson());
    out.set("cpu", cpu.toJson());
    out.set("memory", memory.toJson());

    json::Value diskList = json::Value::array();
    for (const DiskUsage& disk : disks) {
        diskList.push(disk.toJson());
    }
    out.set("disks", diskList);

    json::Value interfaceList = json::Value::array();
    for (const NetworkInterface& item : interfaces) {
        interfaceList.push(item.toJson());
    }
    out.set("network_interfaces", interfaceList);

    if (!topProcesses.empty()) {
        json::Value processList = json::Value::array();
        for (const ProcessInfo& process : topProcesses) {
            processList.push(process.toJson());
        }
        out.set("top_processes", processList);
    }

    json::Value environmentJson = json::Value::object();
    for (const auto& [key, value] : environment) {
        environmentJson.set(key, value);
    }
    out.set("environment", environmentJson);

    setIfNotEmpty(out, "executable_path", executablePath);
    setIfNotEmpty(out, "working_directory", workingDirectory);
    out.set("process_id", processId);

    if (!unavailable.empty()) {
        json::Value notes = json::Value::array();
        for (const std::string& note : unavailable) {
            notes.push(note);
        }
        out.set("unavailable", notes);
    }
    return out;
}

json::Value GpuProcessInfo::toJson() const {
    json::Value out = json::Value::object();
    out.set("pid", pid);
    out.set("name", name);
    setIfKnown(out, "used_memory_mb", usedMemoryMb);
    return out;
}

json::Value GpuDevice::toJson() const {
    json::Value out = json::Value::object();
    out.set("index", index);
    out.set("name", name);
    setIfNotEmpty(out, "uuid", uuid);
    setIfNotEmpty(out, "driver_version", driverVersion);
    setIfNotEmpty(out, "cuda_version", cudaVersion);
    setIfKnown(out, "memory_total_mb", memoryTotalMb);
    setIfKnown(out, "memory_used_mb", memoryUsedMb);
    setIfKnown(out, "memory_free_mb", memoryFreeMb);
    setIfKnown(out, "utilization_percent", utilizationPercent);
    setIfKnown(out, "memory_utilization_percent", memoryUtilizationPercent);
    setIfKnown(out, "temperature_celsius", temperatureCelsius);
    setIfKnown(out, "power_draw_watts", powerDrawWatts);
    if (!processes.empty()) {
        json::Value list = json::Value::array();
        for (const GpuProcessInfo& process : processes) {
            list.push(process.toJson());
        }
        out.set("processes", list);
    }
    return out;
}

json::Value GpuSnapshot::toJson() const {
    json::Value out = json::Value::object();
    out.set("available", available);
    out.set("source", source);
    // Always emitted, never omitted: a consumer must be able to tell mock data
    // from a measurement without knowing which provider produced it.
    out.set("is_mock_data", isMockData);
    if (!available) {
        out.set("unavailable_reason", unavailableReason);
    }
    setIfNotEmpty(out, "driver_version", driverVersion);
    setIfNotEmpty(out, "cuda_version", cudaVersion);
    out.set("device_count", static_cast<std::int64_t>(devices.size()));

    json::Value list = json::Value::array();
    for (const GpuDevice& device : devices) {
        list.push(device.toJson());
    }
    out.set("devices", list);
    out.set("summary", summary());
    return out;
}

std::string GpuSnapshot::summary() const {
    if (!available) {
        return "GPU diagnostics unavailable: " + (unavailableReason.empty()
                                                      ? std::string("no NVIDIA GPU detected.")
                                                      : unavailableReason);
    }

    std::ostringstream os;
    if (isMockData) {
        os << "[MOCK TEST DATA] ";
    }
    if (devices.empty()) {
        os << "GPU driver present but no devices reported";
        return os.str();
    }

    os << devices.size() << "x " << devices.front().name;
    if (!driverVersion.empty()) {
        os << " (driver " << driverVersion;
        if (!cudaVersion.empty()) {
            os << ", CUDA " << cudaVersion;
        }
        os << ')';
    }
    return os.str();
}

}  // namespace testforge::diagnostics
