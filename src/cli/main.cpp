#include "CliOutput.hpp"
#include "CommandLine.hpp"
#include "Commands.hpp"

#include "testforge/core/Environment.hpp"
#include "testforge/core/Exceptions.hpp"
#include "testforge/core/Interrupt.hpp"
#include "testforge/core/Logger.hpp"
#include "testforge/core/StringUtils.hpp"
#include "testforge/core/Version.hpp"

#include <atomic>
#include <csignal>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>

namespace {

using namespace testforge;
using testforge::cli::CliOutput;
using testforge::cli::CommandLine;
using testforge::cli::Commands;
using testforge::cli::ParsedCommand;
namespace exitcode = testforge::cli::exitcode;

/// Builds the configuration from, in increasing precedence: defaults, the
/// config file, environment variables, command-line flags.
Config buildConfig(const ParsedCommand& parsed) {
    // A .env file is a convenience for local development; the real environment
    // always wins, and nothing in it is ever logged.
    env::loadDotEnv(".env");

    Config config;
    std::string configPath = parsed.value("config");
    if (configPath.empty()) {
        configPath = env::getOr("TESTFORGE_CONFIG", "");
    }
    if (configPath.empty() && std::ifstream("config/default.json").good()) {
        configPath = "config/default.json";
    }
    if (!configPath.empty()) {
        config = Config::loadFromFile(configPath);
    }

    config.applyEnvironmentOverrides();

    if (const std::string level = parsed.value("log-level"); !level.empty()) {
        if (const std::optional<LogLevel> parsedLevel = logLevelFromString(level);
            parsedLevel.has_value()) {
            config.logging.level = *parsedLevel;
        } else {
            throw ConfigurationError("unknown log level '" + level +
                                     "' (trace, debug, info, warn, error, off)");
        }
    }
    if (parsed.flag("verbose")) {
        config.logging.level = LogLevel::Debug;
    }
    if (parsed.flag("quiet")) {
        config.logging.level = LogLevel::Error;
    }
    if (parsed.has("log-file")) {
        config.logging.file = parsed.value("log-file");
    }
    if (parsed.flag("log-json")) {
        config.logging.json = true;
    }
    if (parsed.flag("no-color")) {
        config.logging.color = false;
    }
    if (parsed.has("db")) {
        config.database.path = parsed.value("db");
        config.database.enabled = true;
    }
    if (parsed.flag("no-db") || parsed.flag("no-persist")) {
        config.database.enabled = false;
    }
    if (parsed.has("report-dir")) {
        config.reporting.outputDirectory = parsed.value("report-dir");
    }
    if (parsed.flag("no-report")) {
        config.reporting.json = false;
        config.reporting.html = false;
        config.reporting.junit = false;
    }
    if (parsed.has("timeout")) {
        std::int64_t millis = config.execution.defaultTimeoutMs;
        if (!strings::parseDurationMillis(parsed.value("timeout"), millis)) {
            throw ConfigurationError("could not parse --timeout '" + parsed.value("timeout") +
                                     "' (try 30s, 1500ms, 2m)");
        }
        config.execution.defaultTimeoutMs = millis;
    }

    config.validate();
    return config;
}

void configureLogging(const Config& config) {
    LogManager& manager = LogManager::instance();
    manager.setLevel(config.logging.level);

    std::vector<std::shared_ptr<LogSink>> sinks;
    // Colour is only useful on a terminal; a redirected stream gets plain text.
    const bool color = config.logging.color && CliOutput::stdoutIsTerminal();
    sinks.push_back(std::make_shared<ConsoleLogSink>(config.logging.json, color));
    if (!config.logging.file.empty()) {
        sinks.push_back(std::make_shared<FileLogSink>(config.logging.file));
    }
    manager.replaceSinks(std::move(sinks));

    CliOutput::setColorEnabled(color);
}

}  // namespace

int main(int argc, char** argv) {
    // Ctrl-C must leave the terminal usable and the database consistent, so it
    // sets a flag rather than killing the process mid-write. The commands that
    // can block for a noticeable time poll interrupt::requested().
    interrupt::installHandlers();

    const ParsedCommand parsed = CommandLine::parse(argc, argv);

    if (parsed.error.has_value()) {
        CliOutput::error(*parsed.error);
        return exitcode::kUsageError;
    }
    if (parsed.versionRequested && parsed.command.empty()) {
        std::cout << Version::banner() << '\n';
        return exitcode::kSuccess;
    }
    if (parsed.helpRequested || parsed.command == "help") {
        std::string topic = parsed.command == "help" ? parsed.subcommand : parsed.command;
        if (topic == "help") {
            topic.clear();
        }
        if (topic.empty() && !parsed.positionals.empty()) {
            topic = parsed.positionals.front();
        }
        return Commands::help(topic);
    }

    try {
        const Config config = buildConfig(parsed);
        configureLogging(config);

        if (parsed.command == "version") {
            return Commands::version(parsed);
        }

        api::TestForgeService service(config);
        service.initialise();

        if (parsed.command == "list") {
            return Commands::list(service, parsed);
        }
        if (parsed.command == "run") {
            return Commands::run(service, parsed);
        }
        if (parsed.command == "report") {
            return Commands::report(service, parsed);
        }
        if (parsed.command == "diagnose") {
            return Commands::diagnose(service, parsed);
        }
        if (parsed.command == "history") {
            return Commands::history(service, parsed);
        }
        if (parsed.command == "stats") {
            return Commands::stats(service, parsed);
        }
        if (parsed.command == "serve") {
            return Commands::serve(service, parsed);
        }
        if (parsed.command == "ai") {
            return Commands::ai(service, parsed);
        }
        if (parsed.command == "spec") {
            return Commands::spec(service, parsed);
        }
        if (parsed.command == "db") {
            return Commands::database(service, parsed);
        }

        CliOutput::error("unknown command '" + parsed.command + "'", "run 'testforge help'");
        return exitcode::kUsageError;

    } catch (const ConfigurationError& error) {
        // Configuration problems get their own exit code because they are the
        // user's to fix, not a bug and not a test failure.
        CliOutput::error(error.what(), "check your config file and TESTFORGE_* variables");
        return exitcode::kUsageError;
    } catch (const SecurityError& error) {
        CliOutput::error(error.what(), "see docs/security.md");
        return exitcode::kUsageError;
    } catch (const TestForgeError& error) {
        CliOutput::error(error.what());
        return exitcode::kFrameworkError;
    } catch (const std::exception& error) {
        CliOutput::error(std::string("unexpected failure: ") + error.what(),
                         "this is a bug in TestForge; please report it");
        return exitcode::kFrameworkError;
    } catch (...) {
        CliOutput::error("unexpected failure of unknown type",
                         "this is a bug in TestForge; please report it");
        return exitcode::kFrameworkError;
    }
}
