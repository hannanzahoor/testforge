#pragma once

#include "testforge/core/Json.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace testforge::diagnostics {

/// Every field in this file is optional in practice.
///
/// Diagnostics run on machines TestForge does not control: inside containers
/// with /proc partially masked, under users with no permission to read another
/// process's status, on kernels that do not export a given file. Anything that
/// could not be read stays at its default and is reported as absent rather
/// than guessed at.
struct OsInfo {
    std::string name;           ///< "Linux"
    std::string distribution;   ///< "Ubuntu 22.04.3 LTS", from /etc/os-release
    std::string kernelVersion;  ///< uname -r
    std::string kernelBuild;    ///< uname -v
    std::string hostname;
    std::string architecture;  ///< "x86_64"
    std::int64_t uptimeSeconds = -1;
    bool insideContainer = false;
    std::string containerHint;  ///< what gave the container away

    [[nodiscard]] json::Value toJson() const;
};

struct CpuInfo {
    std::string model;
    int physicalCores = 0;
    int logicalCores = 0;
    double mhz = 0.0;
    double loadAverage1 = -1.0;
    double loadAverage5 = -1.0;
    double loadAverage15 = -1.0;

    /// Sampled over a short window; -1 when it could not be measured.
    double utilizationPercent = -1.0;

    [[nodiscard]] json::Value toJson() const;
};

struct MemoryInfo {
    std::int64_t totalKb = -1;
    std::int64_t availableKb = -1;
    std::int64_t freeKb = -1;
    std::int64_t buffersKb = -1;
    std::int64_t cachedKb = -1;
    std::int64_t swapTotalKb = -1;
    std::int64_t swapFreeKb = -1;

    /// Resident set size of the TestForge process itself — the number that
    /// matters when a run dies to the OOM killer.
    std::int64_t processRssKb = -1;

    [[nodiscard]] double usedPercent() const;

    [[nodiscard]] json::Value toJson() const;
};

struct DiskUsage {
    std::string mountPoint;
    std::string filesystem;
    std::int64_t totalBytes = -1;
    std::int64_t availableBytes = -1;

    [[nodiscard]] double usedPercent() const;

    [[nodiscard]] json::Value toJson() const;
};

struct NetworkInterface {
    std::string name;
    std::vector<std::string> addresses;
    bool up = false;
    bool loopback = false;
    std::int64_t rxBytes = -1;
    std::int64_t txBytes = -1;

    [[nodiscard]] json::Value toJson() const;
};

struct ProcessInfo {
    int pid = 0;
    std::string name;
    std::string state;
    std::int64_t rssKb = -1;
    double cpuTimeSeconds = -1.0;

    [[nodiscard]] json::Value toJson() const;
};

/// One complete look at the machine.
struct SystemSnapshot {
    OsInfo os;
    CpuInfo cpu;
    MemoryInfo memory;
    std::vector<DiskUsage> disks;
    std::vector<NetworkInterface> interfaces;
    std::vector<ProcessInfo> topProcesses;
    std::map<std::string, std::string> environment;

    std::string executablePath;
    std::string workingDirectory;
    int processId = 0;

    /// Things that could not be collected, and why. Always populated rather
    /// than silently dropped — "unknown" is a diagnostic finding in itself.
    std::vector<std::string> unavailable;

    [[nodiscard]] json::Value toJson() const;
};

/// One GPU as reported by the driver.
struct GpuProcessInfo {
    int pid = 0;
    std::string name;
    std::int64_t usedMemoryMb = -1;

    [[nodiscard]] json::Value toJson() const;
};

struct GpuDevice {
    int index = 0;
    std::string name;
    std::string uuid;
    std::string driverVersion;
    std::string cudaVersion;
    std::int64_t memoryTotalMb = -1;
    std::int64_t memoryUsedMb = -1;
    std::int64_t memoryFreeMb = -1;
    double utilizationPercent = -1.0;
    double memoryUtilizationPercent = -1.0;
    double temperatureCelsius = -1.0;
    double powerDrawWatts = -1.0;
    std::vector<GpuProcessInfo> processes;

    [[nodiscard]] json::Value toJson() const;
};

/// The result of asking for GPU state.
///
/// `isMockData` exists so that a report can never present synthetic numbers as
/// if they came from hardware. Nothing in TestForge invents GPU data: when
/// there is no NVIDIA GPU, `available` is false and `unavailableReason` says
/// why. See docs/gpu-diagnostics.md.
struct GpuSnapshot {
    bool available = false;
    bool isMockData = false;
    std::string source;             ///< "nvidia-smi", "mock", or "none"
    std::string unavailableReason;  ///< populated when available == false
    std::string driverVersion;
    std::string cudaVersion;
    std::vector<GpuDevice> devices;

    [[nodiscard]] json::Value toJson() const;

    /// Short human line: "2x NVIDIA A100 (driver 550.54.14)" or
    /// "GPU diagnostics unavailable: No NVIDIA GPU detected."
    [[nodiscard]] std::string summary() const;
};

}  // namespace testforge::diagnostics
