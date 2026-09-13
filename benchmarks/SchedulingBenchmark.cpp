/// Scheduling benchmarks.
///
/// This measures one thing: how much wall-clock time the worker pool saves on
/// a suite of tests, as a function of worker count. It reports numbers rather
/// than asserting on them, because a threshold that passes on a laptop and
/// fails on a shared CI runner is worse than no threshold at all. The
/// performance *guard rails* live in examples/sample_tests/PerformanceTests.cpp.
///
/// Two workloads, because they behave completely differently:
///
///   io_bound    Tests that sleep. This is what an API suite looks like: the
///               worker is blocked on a socket, so speed-up tracks the worker
///               count well past the core count.
///
///   cpu_bound   Tests that compute. Speed-up is capped by the number of cores,
///               and adding workers past that point buys nothing.
///
/// Reporting both is the point. A single "3.9x faster" headline would be true
/// of one workload and misleading about the other.
///
/// Usage:
///   ./build/testforge_bench                       # default sweep
///   ./build/testforge_bench --tests 64 --json out.json
///   ./build/testforge_bench --workload cpu

#include "testforge/core/Config.hpp"
#include "testforge/core/Json.hpp"
#include "testforge/core/Logger.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/core/TestContext.hpp"
#include "testforge/core/Version.hpp"
#include "testforge/execution/TestRunner.hpp"
#include "testforge/testing/TestRegistry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace testforge;

namespace {

struct Options {
    int testCount = 48;
    int workPerTestMs = 40;
    int repeats = 3;
    std::vector<int> workerCounts;
    std::string workload = "both";  // io | cpu | both
    std::string jsonPath;
};

/// Deterministic busy-work. Volatile so the optimiser cannot delete it, and
/// calibrated at run time so the benchmark is comparable across machines.
std::uint64_t burn(std::uint64_t iterations) {
    volatile std::uint64_t accumulator = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        accumulator = accumulator * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    return accumulator;
}

/// Finds how many burn() iterations take roughly `targetMs`, so the CPU-bound
/// workload does comparable work on a fast and a slow machine.
std::uint64_t calibrate(int targetMs) {
    std::uint64_t iterations = 1'000'000;
    for (int attempt = 0; attempt < 12; ++attempt) {
        const Stopwatch watch;
        (void)burn(iterations);
        const std::int64_t elapsed = watch.elapsed().count();
        if (elapsed >= targetMs) {
            return elapsed > 0 ? iterations * static_cast<std::uint64_t>(targetMs) /
                                     static_cast<std::uint64_t>(elapsed)
                               : iterations;
        }
        iterations *= 2;
    }
    return iterations;
}

void registerWorkload(
    const std::string& suite, int count, int workMs, std::uint64_t burnIterations, bool cpuBound) {
    TestRegistry::instance().removeSuite(suite);
    for (int i = 0; i < count; ++i) {
        TestMetadata metadata;
        metadata.suite = suite;
        metadata.name = "t" + std::to_string(i);
        metadata.tags = {suite};

        TestRegistry::instance().registerOrReplace(
            metadata, [metadata, workMs, burnIterations, cpuBound]() -> TestCasePtr {
                return std::make_unique<FunctionTestCase>(
                    metadata, [workMs, burnIterations, cpuBound](TestContext& ctx) {
                        if (cpuBound) {
                            (void)burn(burnIterations);
                        } else {
                            (void)ctx.sleepFor(Milliseconds{workMs});
                        }
                    });
            });
    }
}

struct Measurement {
    int workers = 0;
    std::vector<std::int64_t> wallMs;
    std::int64_t testTimeMs = 0;
    int tests = 0;

    [[nodiscard]] std::int64_t best() const {
        return wallMs.empty() ? 0 : *std::min_element(wallMs.begin(), wallMs.end());
    }

    [[nodiscard]] double median() const {
        if (wallMs.empty()) {
            return 0.0;
        }
        std::vector<std::int64_t> sorted = wallMs;
        std::sort(sorted.begin(), sorted.end());
        return static_cast<double>(sorted[sorted.size() / 2]);
    }
};

Measurement measure(const Config& config, const std::string& suite, int workers, int repeats) {
    Measurement measurement;
    measurement.workers = workers;

    for (int repeat = 0; repeat < repeats; ++repeat) {
        TestRunner runner(config, TestRegistry::instance());

        RunOptions options;
        options.filter.suites = {suite};
        options.workers = workers;
        options.persist = false;
        options.collectDiagnostics = false;

        const Stopwatch watch;
        const TestRun run = runner.run(options);
        measurement.wallMs.push_back(watch.elapsed().count());

        measurement.tests = static_cast<int>(run.results.size());
        measurement.testTimeMs = run.statistics().totalDuration.count();
    }
    return measurement;
}

std::string formatDouble(double value, int precision = 2) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(precision);
    os << value;
    return os.str();
}

void printTable(const std::string& title, const std::vector<Measurement>& measurements, int cores) {
    if (measurements.empty()) {
        return;
    }
    const double baseline = measurements.front().median();

    std::cout << "\n" << title << "\n";
    std::cout << std::string(78, '-') << "\n";
    std::cout << "  workers   wall (median)   wall (best)   speed-up   efficiency\n";
    std::cout << std::string(78, '-') << "\n";

    for (const Measurement& measurement : measurements) {
        const double median = measurement.median();
        const double speedup = median > 0.0 ? baseline / median : 0.0;
        // Efficiency against the *cores available*, not the worker count: 8
        // workers on 4 cores cannot exceed 4x on CPU-bound work, and saying so
        // is more useful than reporting 50% efficiency as if it were waste.
        const double ceiling =
            std::min(static_cast<double>(measurement.workers), static_cast<double>(cores));
        const double efficiency = ceiling > 0.0 ? 100.0 * speedup / ceiling : 0.0;

        std::cout << "  " << std::setw(7) << measurement.workers << "   " << std::setw(13)
                  << (formatDouble(median, 0) + "ms") << "   " << std::setw(11)
                  << (std::to_string(measurement.best()) + "ms") << "   " << std::setw(8)
                  << (formatDouble(speedup) + "x") << "   " << std::setw(9)
                  << (formatDouble(efficiency, 0) + "%") << "\n";
    }
    std::cout << std::string(78, '-') << "\n";
}

json::Value toJson(const std::string& name, const std::vector<Measurement>& measurements) {
    json::Value out = json::Value::object();
    out.set("workload", name);

    const double baseline = measurements.empty() ? 0.0 : measurements.front().median();
    json::Value points = json::Value::array();
    for (const Measurement& measurement : measurements) {
        json::Value point = json::Value::object();
        point.set("workers", measurement.workers);
        point.set("tests", measurement.tests);
        point.set("wall_median_ms", measurement.median());
        point.set("wall_best_ms", measurement.best());
        point.set("sum_of_test_durations_ms", measurement.testTimeMs);
        point.set("speedup_vs_one_worker",
                  measurement.median() > 0.0 ? baseline / measurement.median() : 0.0);
        points.push(point);
    }
    out.set("measurements", points);
    return out;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto next = [&](int& index) -> std::string {
            return index + 1 < argc ? argv[++index] : std::string{};
        };

        if (argument == "--tests") {
            std::int64_t value = options.testCount;
            strings::parseInt(next(i), value);
            options.testCount = static_cast<int>(value);
        } else if (argument == "--work-ms") {
            std::int64_t value = options.workPerTestMs;
            strings::parseInt(next(i), value);
            options.workPerTestMs = static_cast<int>(value);
        } else if (argument == "--repeats") {
            std::int64_t value = options.repeats;
            strings::parseInt(next(i), value);
            options.repeats = static_cast<int>(value);
        } else if (argument == "--workers") {
            for (const std::string& piece : strings::split(next(i), ',', true)) {
                std::int64_t value = 0;
                if (strings::parseInt(piece, value) && value > 0) {
                    options.workerCounts.push_back(static_cast<int>(value));
                }
            }
        } else if (argument == "--workload") {
            options.workload = next(i);
        } else if (argument == "--json") {
            options.jsonPath = next(i);
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "usage: testforge_bench [--tests N] [--work-ms N] [--repeats N]\n"
                      << "                       [--workers 1,2,4,8] [--workload io|cpu|both]\n"
                      << "                       [--json PATH]\n";
            std::exit(0);
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parseOptions(argc, argv);

    // Logging off: the benchmark measures scheduling, not stderr throughput.
    LogManager::instance().setLevel(LogLevel::Off);

    const unsigned hardware = std::thread::hardware_concurrency();
    const int cores = hardware == 0 ? 4 : static_cast<int>(hardware);

    std::vector<int> workerCounts = options.workerCounts;
    if (workerCounts.empty()) {
        // 1, 2, 4, ... up to twice the core count: past the cores is where the
        // difference between the two workloads becomes obvious.
        for (int workers = 1; workers <= std::max(2, cores * 2); workers *= 2) {
            workerCounts.push_back(workers);
        }
    }

    Config config;
    config.database.enabled = false;
    config.reporting.console = false;
    config.reporting.json = false;
    config.reporting.html = false;
    config.diagnostics.collectOnFailure = false;
    config.execution.defaultTimeoutMs = 120'000;

    std::cout << "TestForge scheduling benchmark\n";
    std::cout << "  host cores ........ " << cores << "\n";
    std::cout << "  tests ............. " << options.testCount << "\n";
    std::cout << "  work per test ..... " << options.workPerTestMs << "ms\n";
    std::cout << "  repeats ........... " << options.repeats << " (median reported)\n";
    std::cout << "  build ............. " << Version::banner() << "\n";

    json::Value report = json::Value::object();
    report.set("cores", cores);
    report.set("tests", options.testCount);
    report.set("work_per_test_ms", options.workPerTestMs);
    report.set("repeats", options.repeats);
    report.set("build", Version::banner());
    json::Value workloads = json::Value::array();

    if (options.workload == "io" || options.workload == "both") {
        registerWorkload("bench_io", options.testCount, options.workPerTestMs, 0, false);
        std::vector<Measurement> measurements;
        for (const int workers : workerCounts) {
            measurements.push_back(measure(config, "bench_io", workers, options.repeats));
        }
        printTable("I/O-bound workload (each test sleeps; models an API suite)",
                   measurements,
                   // Sleeping workers are not competing for a core, so the
                   // ceiling is the worker count, not the core count.
                   *std::max_element(workerCounts.begin(), workerCounts.end()));
        workloads.push(toJson("io_bound", measurements));
    }

    if (options.workload == "cpu" || options.workload == "both") {
        std::cout << "\ncalibrating CPU workload... " << std::flush;
        const std::uint64_t iterations = calibrate(options.workPerTestMs);
        std::cout << iterations << " iterations per test\n";

        registerWorkload("bench_cpu", options.testCount, options.workPerTestMs, iterations, true);
        std::vector<Measurement> measurements;
        for (const int workers : workerCounts) {
            measurements.push_back(measure(config, "bench_cpu", workers, options.repeats));
        }
        printTable(
            "CPU-bound workload (each test computes; capped by core count)", measurements, cores);
        workloads.push(toJson("cpu_bound", measurements));
    }

    report.set("workloads", workloads);
    report.set("note",
               "Measured on this machine at this moment. Speed-up on I/O-bound work is limited "
               "by the worker count; on CPU-bound work it is limited by the core count. Numbers "
               "from a shared CI runner are not comparable with numbers from an idle laptop.");

    if (!options.jsonPath.empty()) {
        std::ofstream file(options.jsonPath);
        if (file.is_open()) {
            file << report.dump(2) << "\n";
            std::cout << "\nJSON written to " << options.jsonPath << "\n";
        } else {
            std::cerr << "could not write " << options.jsonPath << "\n";
            return 1;
        }
    }

    std::cout << "\nSpeed-up is median wall time at N workers divided by median wall time at 1.\n";
    return 0;
}
