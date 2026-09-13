#include "testforge/core/Logger.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/reporting/Reporter.hpp"

#include <filesystem>
#include <fstream>
#include <utility>

namespace testforge::reporting {
namespace {

/// Timestamp fragment safe for a filename on every filesystem: no colons.
std::string filenameTimestamp(TimePoint when) {
    std::string text = toIso8601(when);
    text = strings::replaceAll(text, ":", "");
    text = strings::replaceAll(text, "-", "");
    text = strings::replaceAll(text, ".", "");
    return text;
}

}  // namespace

ReportWriter::ReportWriter(ReportingConfig config) : config_(std::move(config)) {}

void ReportWriter::addReporter(std::shared_ptr<Reporter> reporter) {
    if (reporter) {
        extra_.push_back(std::move(reporter));
    }
}

bool ReportWriter::prepareDirectory() const {
    if (config_.outputDirectory.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(config_.outputDirectory, error);
    if (error) {
        Logger("reporting")
            .error("cannot create the report directory",
                   {{"path", config_.outputDirectory}, {"error", error.message()}});
        return false;
    }
    return true;
}

std::string ReportWriter::pathFor(const TestRun& run, const Reporter& reporter) const {
    std::filesystem::path path(config_.outputDirectory);
    // Run id first so an alphabetical listing groups a run's artefacts, and the
    // timestamp second so the newest is easy to spot.
    path /= "run-" + run.runId.substr(0, 8) + "-" + filenameTimestamp(run.startedAt) + "." +
            std::string(reporter.fileExtension());
    return path.string();
}

std::vector<std::string> ReportWriter::write(const TestRun& run) const {
    std::vector<std::string> written;

    std::vector<std::shared_ptr<Reporter>> reporters;
    if (config_.json) {
        reporters.push_back(std::make_shared<JsonReporter>(true));
    }
    if (config_.html) {
        HtmlReporter::Options options;
        options.title = run.label.empty() ? "TestForge Report" : "TestForge — " + run.label;
        reporters.push_back(std::make_shared<HtmlReporter>(std::move(options)));
    }
    if (config_.junit) {
        reporters.push_back(std::make_shared<JUnitReporter>());
    }
    for (const auto& reporter : extra_) {
        reporters.push_back(reporter);
    }

    if (reporters.empty()) {
        return written;
    }
    if (!prepareDirectory()) {
        return written;
    }

    Logger logger("reporting");
    for (const auto& reporter : reporters) {
        const std::string path = pathFor(run, *reporter);
        try {
            const std::string content = reporter->render(run);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                logger.error("cannot open a report file for writing",
                             {{"path", path}, {"reporter", std::string(reporter->name())}});
                continue;
            }
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!file) {
                logger.error("failed while writing a report", {{"path", path}});
                continue;
            }
            written.push_back(path);
            logger.info("report written",
                        {{"path", path},
                         {"reporter", std::string(reporter->name())},
                         {"bytes", static_cast<std::int64_t>(content.size())}});
        } catch (const std::exception& error) {
            // A failed report must never turn a passing run into a failure.
            logger.error("reporter threw",
                         {{"reporter", std::string(reporter->name())},
                          {"error", std::string(error.what())}});
        }
    }

    // A stable "latest" alias makes scripts and the dashboard simpler: they can
    // point at one path instead of globbing for the newest file.
    if (!written.empty()) {
        for (const auto& reporter : reporters) {
            const std::filesystem::path source = pathFor(run, *reporter);
            if (!std::filesystem::exists(source)) {
                continue;
            }
            std::filesystem::path alias(config_.outputDirectory);
            alias /= std::string("latest.") + std::string(reporter->fileExtension());
            std::error_code error;
            std::filesystem::copy_file(
                source, alias, std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                logger.debug("could not update the latest alias",
                             {{"path", alias.string()}, {"error", error.message()}});
            }
        }
    }

    return written;
}

}  // namespace testforge::reporting
