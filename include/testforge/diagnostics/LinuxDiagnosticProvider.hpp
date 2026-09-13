#pragma once

#include "testforge/diagnostics/DiagnosticProvider.hpp"

#include <string>

namespace testforge::diagnostics {

/// Reads machine state from the Linux kernel's own interfaces.
///
/// Everything comes from /proc, /sys and a handful of POSIX calls — no shelling
/// out to `top`, `free` or `df`. Parsing files directly is faster, has no
/// dependency on which coreutils flavour is installed, cannot be affected by
/// locale, and removes an entire class of command-injection risk.
///
/// The one exception is `journalctl` for system logs, which has no file
/// equivalent; it goes through ProcessRunner with a fixed argument list and is
/// off by default because it usually needs privileges.
///
/// On a non-Linux POSIX system the provider still reports what it can (uname,
/// hostname, filesystem usage) and lists the rest as unavailable.
class LinuxDiagnosticProvider final : public SystemDiagnosticProvider {
 public:
    [[nodiscard]] std::string_view name() const override { return "system"; }

    [[nodiscard]] bool isAvailable() const override;

    [[nodiscard]] json::Value collect(const DiagnosticsConfig& config) const noexcept override;

    [[nodiscard]] SystemSnapshot snapshot(const DiagnosticsConfig& config) const override;

    // --- individual collectors, public so they can be tested in isolation ---

    [[nodiscard]] static OsInfo readOsInfo();

    [[nodiscard]] static CpuInfo readCpuInfo();

    [[nodiscard]] static MemoryInfo readMemoryInfo();

    [[nodiscard]] static std::vector<DiskUsage> readDiskUsage();

    [[nodiscard]] static std::vector<NetworkInterface> readNetworkInterfaces();

    [[nodiscard]] static std::vector<ProcessInfo> readTopProcesses(int limit);

    /// Recent kernel/system log lines. Returns an explanatory string rather
    /// than failing when the logs are not readable by this user.
    [[nodiscard]] static std::vector<std::string> readRecentSystemLogs(int lines,
                                                                       std::int64_t timeoutMs);
};

}  // namespace testforge::diagnostics
