#pragma once

#include "testforge/diagnostics/DiagnosticProvider.hpp"

#include <string>
#include <vector>

namespace testforge::diagnostics {

/// Queries NVIDIA hardware via `nvidia-smi`.
///
/// Why the CLI and not NVML: linking libnvidia-ml would make the whole build
/// depend on a driver package being installed, which defeats the goal of "runs
/// anywhere, reports what it finds". nvidia-smi is present wherever the driver is,
/// and its `--query-gpu=... --format=csv` mode is a stable, parseable contract.
///
/// The invocation goes through ProcessRunner: a fixed executable name, a fixed
/// argument vector, no shell, and a timeout. Nothing from a test, a config
/// file or a language model ever reaches this command line.
///
/// If there is no GPU, no driver, or nvidia-smi fails, the result is
/// `available == false` with a reason. No values are ever invented.
class NvidiaGpuProvider final : public GpuDiagnosticProvider {
 public:
    [[nodiscard]] std::string_view name() const override { return "gpu"; }

    /// True only when the nvidia-smi binary exists on PATH. Does not run it.
    [[nodiscard]] bool isAvailable() const override;

    [[nodiscard]] json::Value collect(const DiagnosticsConfig& config) const noexcept override;

    [[nodiscard]] GpuSnapshot query(const DiagnosticsConfig& config) const override;

    /// Parses the CSV that `nvidia-smi --query-gpu` produces. Separated from
    /// the process invocation so it can be unit-tested against captured
    /// output on machines with no GPU.
    [[nodiscard]] static std::vector<GpuDevice> parseQueryCsv(const std::string& csv);

    /// Parses `nvidia-smi --query-compute-apps` output.
    [[nodiscard]] static std::vector<GpuProcessInfo> parseComputeAppsCsv(const std::string& csv);

    /// The exact argument vector used, exposed for the docs and for tests.
    [[nodiscard]] static std::vector<std::string> queryArguments();
};

/// Returns fabricated GPU data, clearly marked as such.
///
/// Exists so the GPU-dependent code paths — parsing, reporting, the REST
/// endpoint, the AI failure context — can be tested on a machine with no
/// NVIDIA hardware, which is every machine in this project's CI.
///
/// Every snapshot it produces has `isMockData == true` and `source == "mock"`,
/// and every reporter surfaces that flag. Mock data can never be mistaken for
/// a measurement.
class MockGpuProvider final : public GpuDiagnosticProvider {
 public:
    /// Constructs a provider reporting `deviceCount` synthetic devices.
    explicit MockGpuProvider(int deviceCount = 1, std::string deviceName = "MOCK-GPU-0");

    [[nodiscard]] std::string_view name() const override { return "gpu"; }

    [[nodiscard]] bool isAvailable() const override { return true; }

    [[nodiscard]] json::Value collect(const DiagnosticsConfig& config) const noexcept override;

    [[nodiscard]] GpuSnapshot query(const DiagnosticsConfig& config) const override;

    /// Makes the mock report no hardware, for testing the unavailable path.
    void setUnavailable(std::string reason);

 private:
    int deviceCount_;
    std::string deviceName_;
    std::string unavailableReason_;
};

/// Reports "no GPU" on platforms where no provider applies.
class NullGpuProvider final : public GpuDiagnosticProvider {
 public:
    explicit NullGpuProvider(std::string reason = "no GPU diagnostic provider is configured");

    [[nodiscard]] std::string_view name() const override { return "gpu"; }

    [[nodiscard]] bool isAvailable() const override { return false; }

    [[nodiscard]] json::Value collect(const DiagnosticsConfig& config) const noexcept override;

    [[nodiscard]] GpuSnapshot query(const DiagnosticsConfig& config) const override;

 private:
    std::string reason_;
};

/// Picks the right provider for this machine: NvidiaGpuProvider when
/// nvidia-smi is on PATH, NullGpuProvider otherwise.
///
/// Never returns MockGpuProvider. Mock data is only ever installed explicitly
/// by a test.
std::shared_ptr<GpuDiagnosticProvider> createDefaultGpuProvider();

}  // namespace testforge::diagnostics
