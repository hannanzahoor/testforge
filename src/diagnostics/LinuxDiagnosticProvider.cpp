#include "testforge/diagnostics/LinuxDiagnosticProvider.hpp"

#include "testforge/core/Environment.hpp"
#include "testforge/core/Process.hpp"
#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace testforge::diagnostics {
namespace {

/// Reads a whole file. Returns nullopt when it does not exist or cannot be
/// read — which is normal inside containers and under unprivileged users.
std::optional<std::string> readFile(const std::string& path, std::size_t maxBytes = 1u << 20u) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string contents;
    contents.resize(maxBytes);
    file.read(contents.data(), static_cast<std::streamsize>(maxBytes));
    contents.resize(static_cast<std::size_t>(file.gcount()));
    return contents;
}

std::optional<std::string> readFirstLine(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string line;
    if (!std::getline(file, line)) {
        return std::nullopt;
    }
    return line;
}

/// Extracts the value from a "Key: 12345 kB" line in /proc/meminfo.
std::int64_t parseMemInfoValue(std::string_view content, std::string_view key) {
    std::size_t pos = 0;
    while (pos < content.size()) {
        const std::size_t lineEnd = content.find('\n', pos);
        const std::string_view line = content.substr(
            pos, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - pos);
        if (strings::startsWith(line, key) && line.size() > key.size() && line[key.size()] == ':') {
            std::string digits;
            for (const char c : line.substr(key.size() + 1)) {
                if (c >= '0' && c <= '9') {
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
        if (lineEnd == std::string_view::npos) {
            break;
        }
        pos = lineEnd + 1;
    }
    return -1;
}

/// Aggregate jiffies from a /proc/stat "cpu ..." line: (busy, total).
bool readCpuJiffies(std::int64_t& busy, std::int64_t& total) {
    const std::optional<std::string> line = readFirstLine("/proc/stat");
    if (!line.has_value() || !strings::startsWith(*line, "cpu ")) {
        return false;
    }
    const std::vector<std::string> fields = strings::split(*line, ' ', true);
    if (fields.size() < 5) {
        return false;
    }
    std::int64_t sum = 0;
    std::int64_t idle = 0;
    for (std::size_t i = 1; i < fields.size(); ++i) {
        std::int64_t value = 0;
        if (!strings::parseInt(fields[i], value)) {
            continue;
        }
        sum += value;
        // Fields 4 and 5 are idle and iowait.
        if (i == 4 || i == 5) {
            idle += value;
        }
    }
    total = sum;
    busy = sum - idle;
    return total > 0;
}

std::string unquote(std::string_view text) {
    std::string value = strings::trim(text);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

}  // namespace

bool LinuxDiagnosticProvider::isAvailable() const {
#if defined(_WIN32)
    return false;
#else
    return true;
#endif
}

OsInfo LinuxDiagnosticProvider::readOsInfo() {
    OsInfo info;
#if !defined(_WIN32)
    ::utsname uts{};
    if (::uname(&uts) == 0) {
        info.name = uts.sysname;
        info.kernelVersion = uts.release;
        info.kernelBuild = uts.version;
        info.hostname = uts.nodename;
        info.architecture = uts.machine;
    }

    // /etc/os-release is the standard, machine-readable distribution identity.
    if (const std::optional<std::string> release = readFile("/etc/os-release", 8192);
        release.has_value()) {
        for (const std::string& line : strings::splitLines(*release)) {
            if (strings::startsWith(line, "PRETTY_NAME=")) {
                info.distribution = unquote(line.substr(12));
                break;
            }
        }
    }

    if (const std::optional<std::string> uptime = readFirstLine("/proc/uptime");
        uptime.has_value()) {
        double seconds = 0.0;
        const std::vector<std::string> fields = strings::split(*uptime, ' ', true);
        if (!fields.empty() && strings::parseDouble(fields[0], seconds)) {
            info.uptimeSeconds = static_cast<std::int64_t>(seconds);
        }
    }

    // Container detection. None of these signals is authoritative on its own,
    // so the hint records which one fired rather than claiming certainty.
    if (readFile("/.dockerenv", 1).has_value()) {
        info.insideContainer = true;
        info.containerHint = "/.dockerenv exists";
    } else if (const std::optional<std::string> cgroup = readFile("/proc/1/cgroup", 65536);
               cgroup.has_value() &&
               (strings::contains(*cgroup, "docker") || strings::contains(*cgroup, "kubepods") ||
                strings::contains(*cgroup, "containerd"))) {
        info.insideContainer = true;
        info.containerHint = "container runtime referenced in /proc/1/cgroup";
    } else if (env::isSet("container")) {
        info.insideContainer = true;
        info.containerHint = "the 'container' environment variable is set";
    }

    // WSL is worth calling out explicitly: it behaves like Linux but has no
    // GPU passthrough by default and a virtualised clock, both of which change
    // how a failure should be read.
    if (strings::containsIgnoreCase(info.kernelVersion, "microsoft") ||
        strings::containsIgnoreCase(info.kernelBuild, "wsl")) {
        info.containerHint += info.containerHint.empty() ? "" : "; ";
        info.containerHint += "running under WSL";
    }
#endif
    return info;
}

CpuInfo LinuxDiagnosticProvider::readCpuInfo() {
    CpuInfo info;
#if !defined(_WIN32)
    const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0) {
        info.logicalCores = static_cast<int>(online);
    }

    if (const std::optional<std::string> cpuinfo = readFile("/proc/cpuinfo", 1u << 20u);
        cpuinfo.has_value()) {
        std::vector<std::string> coreIds;
        for (const std::string& line : strings::splitLines(*cpuinfo)) {
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const std::string key = strings::trim(line.substr(0, colon));
            const std::string value = strings::trim(line.substr(colon + 1));
            if (info.model.empty() && (key == "model name" || key == "Model")) {
                info.model = value;
            } else if (info.mhz <= 0.0 && key == "cpu MHz") {
                double mhz = 0.0;
                if (strings::parseDouble(value, mhz)) {
                    info.mhz = mhz;
                }
            } else if (key == "core id") {
                if (std::find(coreIds.begin(), coreIds.end(), value) == coreIds.end()) {
                    coreIds.push_back(value);
                }
            }
        }
        if (!coreIds.empty()) {
            info.physicalCores = static_cast<int>(coreIds.size());
        }
    }

    if (const std::optional<std::string> loadavg = readFirstLine("/proc/loadavg");
        loadavg.has_value()) {
        const std::vector<std::string> fields = strings::split(*loadavg, ' ', true);
        if (fields.size() >= 3) {
            strings::parseDouble(fields[0], info.loadAverage1);
            strings::parseDouble(fields[1], info.loadAverage5);
            strings::parseDouble(fields[2], info.loadAverage15);
        }
    }

    // Utilisation needs two samples; 120ms keeps the cost of a diagnostic
    // collection negligible while still producing a usable number.
    std::int64_t busy1 = 0;
    std::int64_t total1 = 0;
    if (readCpuJiffies(busy1, total1)) {
        const ::timespec nap{0, 120L * 1000L * 1000L};
        ::nanosleep(&nap, nullptr);
        std::int64_t busy2 = 0;
        std::int64_t total2 = 0;
        if (readCpuJiffies(busy2, total2) && total2 > total1) {
            info.utilizationPercent =
                100.0 * static_cast<double>(busy2 - busy1) / static_cast<double>(total2 - total1);
        }
    }
#endif
    return info;
}

MemoryInfo LinuxDiagnosticProvider::readMemoryInfo() {
    MemoryInfo info;
#if !defined(_WIN32)
    if (const std::optional<std::string> meminfo = readFile("/proc/meminfo", 65536);
        meminfo.has_value()) {
        info.totalKb = parseMemInfoValue(*meminfo, "MemTotal");
        info.availableKb = parseMemInfoValue(*meminfo, "MemAvailable");
        info.freeKb = parseMemInfoValue(*meminfo, "MemFree");
        info.buffersKb = parseMemInfoValue(*meminfo, "Buffers");
        info.cachedKb = parseMemInfoValue(*meminfo, "Cached");
        info.swapTotalKb = parseMemInfoValue(*meminfo, "SwapTotal");
        info.swapFreeKb = parseMemInfoValue(*meminfo, "SwapFree");
    }
    if (const std::optional<std::string> status = readFile("/proc/self/status", 65536);
        status.has_value()) {
        info.processRssKb = parseMemInfoValue(*status, "VmRSS");
    }
#endif
    return info;
}

std::vector<DiskUsage> LinuxDiagnosticProvider::readDiskUsage() {
    std::vector<DiskUsage> disks;
#if !defined(_WIN32)
    // Only real, locally-backed filesystems: tmpfs/cgroup/proc entries would
    // triple the size of the report without helping anyone.
    static constexpr std::array<std::string_view, 8> kIgnoredTypes = {
        "proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2", "securityfs", "tracefs"};

    std::vector<std::pair<std::string, std::string>> mounts;  // (device, mountpoint)
    if (const std::optional<std::string> mountinfo = readFile("/proc/mounts", 1u << 18u);
        mountinfo.has_value()) {
        for (const std::string& line : strings::splitLines(*mountinfo)) {
            const std::vector<std::string> fields = strings::split(line, ' ', true);
            if (fields.size() < 3) {
                continue;
            }
            const std::string& type = fields[2];
            if (std::find(kIgnoredTypes.begin(), kIgnoredTypes.end(), type) !=
                kIgnoredTypes.end()) {
                continue;
            }
            if (strings::startsWith(fields[1], "/sys") || strings::startsWith(fields[1], "/proc")) {
                continue;
            }
            mounts.emplace_back(fields[0], fields[1]);
        }
    }
    if (mounts.empty()) {
        mounts.emplace_back("", "/");
    }

    for (const auto& [device, mountPoint] : mounts) {
        struct ::statvfs stats {};

        if (::statvfs(mountPoint.c_str(), &stats) != 0) {
            continue;
        }
        if (stats.f_blocks == 0) {
            continue;
        }
        DiskUsage usage;
        usage.mountPoint = mountPoint;
        usage.filesystem = device;
        usage.totalBytes =
            static_cast<std::int64_t>(stats.f_blocks) * static_cast<std::int64_t>(stats.f_frsize);
        // f_bavail, not f_bfree: the reserved blocks are not available to us.
        usage.availableBytes =
            static_cast<std::int64_t>(stats.f_bavail) * static_cast<std::int64_t>(stats.f_frsize);
        disks.push_back(usage);
        if (disks.size() >= 12) {
            break;
        }
    }
#endif
    return disks;
}

std::vector<NetworkInterface> LinuxDiagnosticProvider::readNetworkInterfaces() {
    std::vector<NetworkInterface> interfaces;
#if !defined(_WIN32)
    ::ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return interfaces;
    }

    for (::ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_name == nullptr) {
            continue;
        }
        const std::string name = entry->ifa_name;

        auto found = std::find_if(
            interfaces.begin(), interfaces.end(), [&name](const NetworkInterface& item) {
                return item.name == name;
            });
        if (found == interfaces.end()) {
            NetworkInterface item;
            item.name = name;
            item.up = (entry->ifa_flags & IFF_UP) != 0;
            item.loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0;

            // Counters live in sysfs rather than in the ifaddrs entry.
            if (const std::optional<std::string> rx =
                    readFirstLine("/sys/class/net/" + name + "/statistics/rx_bytes");
                rx.has_value()) {
                strings::parseInt(*rx, item.rxBytes);
            }
            if (const std::optional<std::string> tx =
                    readFirstLine("/sys/class/net/" + name + "/statistics/tx_bytes");
                tx.has_value()) {
                strings::parseInt(*tx, item.txBytes);
            }

            interfaces.push_back(item);
            found = std::prev(interfaces.end());
        }

        if (entry->ifa_addr == nullptr) {
            continue;
        }
        std::array<char, INET6_ADDRSTRLEN> text{};
        if (entry->ifa_addr->sa_family == AF_INET) {
            const auto* v4 = reinterpret_cast<const ::sockaddr_in*>(entry->ifa_addr);
            if (::inet_ntop(AF_INET, &v4->sin_addr, text.data(), text.size()) != nullptr) {
                found->addresses.emplace_back(text.data());
            }
        } else if (entry->ifa_addr->sa_family == AF_INET6) {
            const auto* v6 = reinterpret_cast<const ::sockaddr_in6*>(entry->ifa_addr);
            if (::inet_ntop(AF_INET6, &v6->sin6_addr, text.data(), text.size()) != nullptr) {
                found->addresses.emplace_back(text.data());
            }
        }
    }

    ::freeifaddrs(list);
#endif
    return interfaces;
}

std::vector<ProcessInfo> LinuxDiagnosticProvider::readTopProcesses(int limit) {
    std::vector<ProcessInfo> processes;
#if !defined(_WIN32)
    ::DIR* proc = ::opendir("/proc");
    if (proc == nullptr) {
        return processes;
    }

    const long ticksPerSecond = ::sysconf(_SC_CLK_TCK);

    while (const ::dirent* entry = ::readdir(proc)) {
        const std::string name = entry->d_name;
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }

        ProcessInfo info;
        std::int64_t pid = 0;
        if (!strings::parseInt(name, pid)) {
            continue;
        }
        info.pid = static_cast<int>(pid);

        const std::optional<std::string> status = readFile("/proc/" + name + "/status", 32768);
        if (!status.has_value()) {
            continue;  // the process exited between readdir and open; normal
        }
        for (const std::string& line : strings::splitLines(*status)) {
            if (strings::startsWith(line, "Name:")) {
                info.name = strings::trim(line.substr(5));
            } else if (strings::startsWith(line, "State:")) {
                info.state = strings::trim(line.substr(6));
            }
        }
        info.rssKb = parseMemInfoValue(*status, "VmRSS");

        if (const std::optional<std::string> stat = readFirstLine("/proc/" + name + "/stat");
            stat.has_value() && ticksPerSecond > 0) {
            // Field 14 (utime) and 15 (stime), 1-based, after the comm field —
            // which may itself contain spaces, so scan from the closing paren.
            const std::size_t closeParen = stat->rfind(')');
            if (closeParen != std::string::npos) {
                const std::vector<std::string> fields =
                    strings::split(stat->substr(closeParen + 1), ' ', true);
                if (fields.size() >= 14) {
                    std::int64_t utime = 0;
                    std::int64_t stime = 0;
                    strings::parseInt(fields[11], utime);
                    strings::parseInt(fields[12], stime);
                    info.cpuTimeSeconds =
                        static_cast<double>(utime + stime) / static_cast<double>(ticksPerSecond);
                }
            }
        }

        processes.push_back(std::move(info));
    }
    ::closedir(proc);

    // Rank by resident memory: an OOM or a leak is the failure mode this list
    // is most often used to diagnose.
    std::sort(processes.begin(), processes.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        return a.rssKb > b.rssKb;
    });
    if (limit > 0 && processes.size() > static_cast<std::size_t>(limit)) {
        processes.resize(static_cast<std::size_t>(limit));
    }
#else
    (void)limit;
#endif
    return processes;
}

std::vector<std::string> LinuxDiagnosticProvider::readRecentSystemLogs(int lines,
                                                                       std::int64_t timeoutMs) {
    std::vector<std::string> out;

    // dmesg first: it is the closest thing to a universal kernel log, and on
    // many systems it is readable without privileges.
    ProcessOptions options;
    options.timeout = Milliseconds{timeoutMs};
    options.maxOutputBytes = 256u * 1024u;

    if (ProcessRunner::isAvailable("journalctl")) {
        const ProcessResult result = ProcessRunner::run(
            "journalctl", {"--no-pager", "-n", std::to_string(lines), "-p", "warning"}, options);
        if (result.ok() && !result.standardOutput.empty()) {
            return strings::splitLines(result.standardOutput);
        }
        if (!result.ok()) {
            out.push_back("journalctl unavailable (" +
                          (result.launchError.empty() ? "exit " + std::to_string(result.exitCode)
                                                      : result.launchError) +
                          ") - this normally means the current user is not in the "
                          "systemd-journal group");
        }
    }

    if (ProcessRunner::isAvailable("dmesg")) {
        const ProcessResult result = ProcessRunner::run("dmesg", {"--level=err,warn"}, options);
        if (result.ok() && !result.standardOutput.empty()) {
            std::vector<std::string> dmesgLines = strings::splitLines(result.standardOutput);
            if (lines > 0 && dmesgLines.size() > static_cast<std::size_t>(lines)) {
                dmesgLines.erase(dmesgLines.begin(),
                                 dmesgLines.end() - static_cast<std::ptrdiff_t>(lines));
            }
            out.insert(out.end(), dmesgLines.begin(), dmesgLines.end());
            return out;
        }
        out.push_back("dmesg unavailable (kernel.dmesg_restrict is commonly set to 1)");
    }

    if (out.empty()) {
        out.emplace_back("no readable system log source found");
    }
    return out;
}

SystemSnapshot LinuxDiagnosticProvider::snapshot(const DiagnosticsConfig& config) const {
    SystemSnapshot snapshot;
    snapshot.os = readOsInfo();
    snapshot.cpu = readCpuInfo();
    snapshot.memory = readMemoryInfo();
    snapshot.disks = readDiskUsage();
    snapshot.interfaces = readNetworkInterfaces();
    snapshot.environment = env::diagnosticSubset();

#if !defined(_WIN32)
    // No cast: pid_t is int on Linux, and -Wconversion is the right place to
    // hear about a platform where it is not.
    snapshot.processId = ::getpid();

    std::array<char, 4096> buffer{};
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0) {
        snapshot.executablePath.assign(buffer.data(), static_cast<std::size_t>(length));
    } else {
        snapshot.unavailable.emplace_back("executable path (/proc/self/exe unreadable)");
    }

    if (::getcwd(buffer.data(), buffer.size()) != nullptr) {
        snapshot.workingDirectory = buffer.data();
    }
#endif

    if (config.includeProcesses) {
        snapshot.topProcesses = readTopProcesses(config.topProcessCount);
        if (snapshot.topProcesses.empty()) {
            snapshot.unavailable.emplace_back("process list (/proc not enumerable)");
        }
    }

    if (snapshot.memory.totalKb < 0) {
        snapshot.unavailable.emplace_back("memory statistics (/proc/meminfo unreadable)");
    }
    if (snapshot.cpu.logicalCores == 0) {
        snapshot.unavailable.emplace_back("CPU topology");
    }
    if (snapshot.disks.empty()) {
        snapshot.unavailable.emplace_back("filesystem usage");
    }

    return snapshot;
}

json::Value LinuxDiagnosticProvider::collect(const DiagnosticsConfig& config) const noexcept {
    try {
        json::Value out = snapshot(config).toJson();
        if (config.includeSystemLogs) {
            json::Value logs = json::Value::array();
            for (const std::string& line : readRecentSystemLogs(40, config.commandTimeoutMs)) {
                logs.push(line);
            }
            out.set("system_logs", logs);
        }
        return out;
    } catch (const std::exception& error) {
        // Diagnostics run while a failure is already being handled; replacing
        // the real failure with a diagnostics failure would be actively
        // unhelpful, so report the problem as data instead.
        json::Value out = json::Value::object();
        out.set("error", std::string("system diagnostics failed: ") + error.what());
        return out;
    } catch (...) {
        json::Value out = json::Value::object();
        out.set("error", "system diagnostics failed with an unknown exception");
        return out;
    }
}

}  // namespace testforge::diagnostics
