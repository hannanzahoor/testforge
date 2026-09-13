#include "testforge/core/Logger.hpp"
#include "testforge/diagnostics/DiagnosticProvider.hpp"
#include "testforge/diagnostics/GpuProviders.hpp"
#include "testforge/diagnostics/LinuxDiagnosticProvider.hpp"

#include <algorithm>
#include <utility>

namespace testforge::diagnostics {

DiagnosticCollector::DiagnosticCollector(DiagnosticsConfig config) : config_(std::move(config)) {}

std::shared_ptr<DiagnosticCollector> DiagnosticCollector::createDefault(
    const DiagnosticsConfig& config) {
    auto collector = std::make_shared<DiagnosticCollector>(config);

    auto system = std::make_shared<LinuxDiagnosticProvider>();
    collector->systemProvider_ = system;
    collector->providers_.push_back(system);

    if (config.includeGpu) {
        // Real hardware when it exists, an explicit "not available" otherwise.
        // Never a mock: synthetic values only enter through a test.
        auto gpu = createDefaultGpuProvider();
        collector->gpuProvider_ = gpu;
        collector->providers_.push_back(gpu);
    }

    return collector;
}

void DiagnosticCollector::addProvider(std::shared_ptr<DiagnosticProvider> provider) {
    if (provider) {
        providers_.push_back(std::move(provider));
    }
}

void DiagnosticCollector::setGpuProvider(std::shared_ptr<GpuDiagnosticProvider> provider) {
    // Drop whatever GPU provider is currently registered so the replacement is
    // the only one that reports under the "gpu" key.
    if (gpuProvider_) {
        providers_.erase(std::remove_if(providers_.begin(),
                                        providers_.end(),
                                        [this](const std::shared_ptr<DiagnosticProvider>& item) {
                                            return item == gpuProvider_;
                                        }),
                         providers_.end());
    }
    gpuProvider_ = std::move(provider);
    if (gpuProvider_) {
        providers_.push_back(gpuProvider_);
    }
}

std::vector<std::string> DiagnosticCollector::providerNames() const {
    std::vector<std::string> names;
    names.reserve(providers_.size());
    for (const auto& provider : providers_) {
        names.emplace_back(provider->name());
    }
    return names;
}

json::Value DiagnosticCollector::collectAll() const noexcept {
    json::Value out = json::Value::object();
    out.set("collected_at", toIso8601(WallClock::now()));

    for (const auto& provider : providers_) {
        try {
            out.set(std::string(provider->name()), provider->collect(config_));
        } catch (const std::exception& error) {
            // collect() is declared noexcept, so reaching here means a provider
            // violated its contract. Record it and carry on with the rest.
            json::Value failure = json::Value::object();
            failure.set("error", std::string(error.what()));
            out.set(std::string(provider->name()), failure);
        } catch (...) {
            json::Value failure = json::Value::object();
            failure.set("error", "provider threw an unknown exception");
            out.set(std::string(provider->name()), failure);
        }
    }
    return out;
}

json::Value DiagnosticCollector::collectForFailure() const noexcept {
    json::Value out = json::Value::object();
    out.set("collected_at", toIso8601(WallClock::now()));

    try {
        if (systemProvider_) {
            // A reduced config for the failure path: process listings and
            // system logs are expensive and are opt-in even when diagnostics
            // are enabled, because a run with 50 failures would otherwise
            // shell out 50 times and store 50 copies of the same log tail.
            DiagnosticsConfig reduced = config_;
            reduced.includeSystemLogs = config_.includeSystemLogs;
            const SystemSnapshot snapshot = systemProvider_->snapshot(reduced);

            json::Value system = json::Value::object();
            system.set("os", snapshot.os.toJson());
            system.set("cpu", snapshot.cpu.toJson());
            system.set("memory", snapshot.memory.toJson());

            json::Value disks = json::Value::array();
            for (const DiskUsage& disk : snapshot.disks) {
                // Only report filesystems that are actually under pressure or
                // that hold the working directory; the rest is noise.
                if (disk.usedPercent() >= 85.0 || disk.mountPoint == "/" ||
                    disk.mountPoint == "/tmp") {
                    disks.push(disk.toJson());
                }
            }
            system.set("disks", disks);

            json::Value interfaces = json::Value::array();
            for (const NetworkInterface& item : snapshot.interfaces) {
                if (item.up) {
                    interfaces.push(item.toJson());
                }
            }
            system.set("network_interfaces", interfaces);

            if (config_.includeProcesses && !snapshot.topProcesses.empty()) {
                json::Value processes = json::Value::array();
                for (const ProcessInfo& process : snapshot.topProcesses) {
                    processes.push(process.toJson());
                }
                system.set("top_processes", processes);
            }

            system.set("working_directory", snapshot.workingDirectory);
            system.set("process_id", snapshot.processId);
            if (!snapshot.unavailable.empty()) {
                json::Value notes = json::Value::array();
                for (const std::string& note : snapshot.unavailable) {
                    notes.push(note);
                }
                system.set("unavailable", notes);
            }
            out.set("system", system);
        }

        if (config_.includeGpu && gpuProvider_) {
            out.set("gpu", gpuProvider_->query(config_).toJson());
        }

        if (config_.includeSystemLogs) {
            json::Value logs = json::Value::array();
            for (const std::string& line :
                 LinuxDiagnosticProvider::readRecentSystemLogs(25, config_.commandTimeoutMs)) {
                logs.push(line);
            }
            out.set("system_logs", logs);
        }
    } catch (const std::exception& error) {
        out.set("error", std::string("diagnostic collection failed: ") + error.what());
    } catch (...) {
        out.set("error", "diagnostic collection failed with an unknown exception");
    }

    return out;
}

GpuSnapshot DiagnosticCollector::gpu() const noexcept {
    if (!gpuProvider_) {
        GpuSnapshot snapshot;
        snapshot.available = false;
        snapshot.source = "none";
        snapshot.unavailableReason = "GPU diagnostics are disabled in the configuration.";
        return snapshot;
    }
    try {
        return gpuProvider_->query(config_);
    } catch (const std::exception& error) {
        GpuSnapshot snapshot;
        snapshot.available = false;
        snapshot.source = "none";
        snapshot.unavailableReason = std::string("GPU query failed: ") + error.what();
        return snapshot;
    }
}

SystemSnapshot DiagnosticCollector::system() const noexcept {
    if (!systemProvider_) {
        SystemSnapshot snapshot;
        snapshot.unavailable.emplace_back("no system diagnostic provider is registered");
        return snapshot;
    }
    try {
        return systemProvider_->snapshot(config_);
    } catch (const std::exception& error) {
        SystemSnapshot snapshot;
        snapshot.unavailable.emplace_back(std::string("system snapshot failed: ") + error.what());
        return snapshot;
    }
}

}  // namespace testforge::diagnostics
