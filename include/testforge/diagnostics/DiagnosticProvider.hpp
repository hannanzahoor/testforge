#pragma once

#include "testforge/core/Config.hpp"
#include "testforge/core/Json.hpp"
#include "testforge/diagnostics/SystemInfo.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace testforge::diagnostics {

/// A source of diagnostic information.
///
/// The interface is intentionally narrow — name, availability, collect — so
/// that adding a provider (container runtime, Kubernetes node, a vendor tool)
/// requires no change to the collector or the runner.
class DiagnosticProvider {
 public:
    virtual ~DiagnosticProvider() = default;

    DiagnosticProvider(const DiagnosticProvider&) = delete;
    DiagnosticProvider& operator=(const DiagnosticProvider&) = delete;
    DiagnosticProvider(DiagnosticProvider&&) = delete;
    DiagnosticProvider& operator=(DiagnosticProvider&&) = delete;

    [[nodiscard]] virtual std::string_view name() const = 0;

    /// Cheap check. Must not run an external command or block.
    [[nodiscard]] virtual bool isAvailable() const = 0;

    /// Collects. Must never throw: diagnostics run while a failure is already
    /// being handled, and an exception here would replace the real failure
    /// with a less interesting one.
    [[nodiscard]] virtual json::Value collect(const DiagnosticsConfig& config) const noexcept = 0;

 protected:
    DiagnosticProvider() = default;
};

/// Reads the machine's own state.
class SystemDiagnosticProvider : public DiagnosticProvider {
 public:
    [[nodiscard]] virtual SystemSnapshot snapshot(const DiagnosticsConfig& config) const = 0;
};

/// Reads GPU state.
///
/// Separated from SystemDiagnosticProvider because GPUs are optional, vendor
/// specific, and frequently there is no such value available — which the
/// GpuSnapshot type is designed to express.
class GpuDiagnosticProvider : public DiagnosticProvider {
 public:
    [[nodiscard]] virtual GpuSnapshot query(const DiagnosticsConfig& config) const = 0;
};

/// Builds the diagnostics blob attached to a failing TestResult.
///
/// Collection is opt-in per run and never happens for a passing test: reading
/// /proc and shelling out to nvidia-smi for thousands of green tests would
/// dominate the runtime of a fast suite.
class DiagnosticCollector {
 public:
    explicit DiagnosticCollector(DiagnosticsConfig config);

    /// Installs the platform providers appropriate for this build.
    static std::shared_ptr<DiagnosticCollector> createDefault(const DiagnosticsConfig& config);

    void addProvider(std::shared_ptr<DiagnosticProvider> provider);

    /// Replaces the GPU provider. Used by tests to inject MockGpuProvider.
    void setGpuProvider(std::shared_ptr<GpuDiagnosticProvider> provider);

    /// Everything, as one JSON object keyed by provider name. Never throws.
    [[nodiscard]] json::Value collectAll() const noexcept;

    /// The subset worth attaching to a failure: enough to explain the failure,
    /// small enough to store next to every failing result.
    [[nodiscard]] json::Value collectForFailure() const noexcept;

    [[nodiscard]] const DiagnosticsConfig& config() const noexcept { return config_; }

    [[nodiscard]] std::vector<std::string> providerNames() const;

    /// Direct access for `testforge diagnose` and the REST endpoint.
    [[nodiscard]] GpuSnapshot gpu() const noexcept;

    [[nodiscard]] SystemSnapshot system() const noexcept;

 private:
    DiagnosticsConfig config_;
    std::vector<std::shared_ptr<DiagnosticProvider>> providers_;
    std::shared_ptr<GpuDiagnosticProvider> gpuProvider_;
    std::shared_ptr<SystemDiagnosticProvider> systemProvider_;
};

}  // namespace testforge::diagnostics
