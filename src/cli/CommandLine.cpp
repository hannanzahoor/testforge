#include "CommandLine.hpp"

#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <map>

namespace testforge::cli {
namespace {

/// Options accepted by every command.
const std::vector<std::string>& globalOptions() {
    static const std::vector<std::string> kOptions = {"config",
                                                      "log-level",
                                                      "log-file",
                                                      "log-json",
                                                      "no-color",
                                                      "color",
                                                      "db",
                                                      "no-db",
                                                      "help",
                                                      "version",
                                                      "quiet",
                                                      "verbose"};
    return kOptions;
}

/// Options that select which tests to act on. Shared by list and run so the
/// two can never disagree about what `--tag` means.
const std::vector<std::string>& selectionOptions() {
    static const std::vector<std::string> kOptions = {"suite",
                                                      "test",
                                                      "tag",
                                                      "exclude-tag",
                                                      "exclude-suite",
                                                      "shard",
                                                      "shards",
                                                      "shuffle",
                                                      "seed",
                                                      "include-disabled"};
    return kOptions;
}

std::vector<std::string> merge(std::vector<std::string> a, const std::vector<std::string>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

/// True for options that take no value.
bool isFlagOption(std::string_view name) {
    static const std::vector<std::string> kFlags = {"help",
                                                    "version",
                                                    "no-color",
                                                    "color",
                                                    "log-json",
                                                    "quiet",
                                                    "verbose",
                                                    "shuffle",
                                                    "fail-fast",
                                                    "no-db",
                                                    "include-disabled",
                                                    "json",
                                                    "no-report",
                                                    "mock",
                                                    "register",
                                                    "watch",
                                                    "open",
                                                    "dry-run",
                                                    "no-persist",
                                                    "gpu",
                                                    "full",
                                                    "prune",
                                                    "all",
                                                    "execute",
                                                    "flaky"};
    return std::find(kFlags.begin(), kFlags.end(), name) != kFlags.end();
}

}  // namespace

bool ParsedCommand::has(std::string_view name) const {
    return options.find(std::string(name)) != options.end();
}

std::string ParsedCommand::value(std::string_view name, std::string_view fallback) const {
    const auto found = options.find(std::string(name));
    if (found == options.end() || found->second.empty()) {
        return std::string(fallback);
    }
    return found->second.back();
}

std::vector<std::string> ParsedCommand::values(std::string_view name) const {
    const auto found = options.find(std::string(name));
    if (found == options.end()) {
        return {};
    }
    // A single occurrence may still carry several comma-separated values.
    std::vector<std::string> out;
    for (const std::string& raw : found->second) {
        for (const std::string& piece : strings::split(raw, ',', true)) {
            const std::string trimmed = strings::trim(piece);
            if (!trimmed.empty()) {
                out.push_back(trimmed);
            }
        }
    }
    return out;
}

int ParsedCommand::intValue(std::string_view name, int fallback) const {
    return static_cast<int>(int64Value(name, fallback));
}

std::int64_t ParsedCommand::int64Value(std::string_view name, std::int64_t fallback) const {
    const std::string raw = value(name);
    std::int64_t parsed = fallback;
    if (!raw.empty() && strings::parseInt(raw, parsed)) {
        return parsed;
    }
    return fallback;
}

bool ParsedCommand::flag(std::string_view name, bool fallback) const {
    const auto found = options.find(std::string(name));
    if (found == options.end()) {
        return fallback;
    }
    if (found->second.empty()) {
        return true;  // bare presence means true
    }
    bool parsed = true;
    if (strings::parseBool(found->second.back(), parsed)) {
        return parsed;
    }
    return true;
}

const std::vector<std::string>& CommandLine::commands() {
    static const std::vector<std::string> kCommands = {"list",
                                                       "run",
                                                       "report",
                                                       "diagnose",
                                                       "history",
                                                       "stats",
                                                       "serve",
                                                       "ai",
                                                       "spec",
                                                       "db",
                                                       "version",
                                                       "help"};
    return kCommands;
}

const std::vector<std::string>& CommandLine::knownOptions(std::string_view command) {
    static const std::map<std::string, std::vector<std::string>> kByCommand = [] {
        std::map<std::string, std::vector<std::string>> table;
        table["list"] = merge(selectionOptions(), {"json", "format"});
        table["run"] = merge(selectionOptions(),
                             {"workers",
                              "timeout",
                              "fail-fast",
                              "retry-failed",
                              "label",
                              "report-dir",
                              "no-report",
                              "json",
                              "no-persist",
                              "dry-run"});
        table["report"] = {"run", "format", "output", "open"};
        table["diagnose"] = {"json", "gpu", "full"};
        table["history"] = {"limit", "test", "json", "prune", "days"};
        table["stats"] = {"runs", "json", "flaky", "limit"};
        table["serve"] = {"host", "port", "workers", "static", "open"};
        table["ai"] = {"requirement",
                       "file",
                       "suite",
                       "max-tests",
                       "register",
                       "run",
                       "test",
                       "json",
                       "mock",
                       "limit"};
        // "execute", not "run": --run already means "which run id" for the report
        // and ai commands, and one flag cannot be both a switch and a value.
        table["spec"] = {"file", "register", "execute", "json", "workers"};
        table["db"] = {"json", "prune", "days", "all"};
        table["version"] = {"json"};
        table["help"] = {};
        return table;
    }();

    static const std::vector<std::string> kEmpty;
    const auto found = kByCommand.find(std::string(command));
    return found == kByCommand.end() ? kEmpty : found->second;
}

ParsedCommand CommandLine::parse(int argc, const char* const* argv) {
    ParsedCommand parsed;

    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(std::max(0, argc - 1)));
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }

    std::size_t index = 0;

    // Leading global flags, before the command word.
    while (index < arguments.size() && strings::startsWith(arguments[index], "-")) {
        const std::string& argument = arguments[index];
        if (argument == "-h" || argument == "--help") {
            parsed.helpRequested = true;
            ++index;
            continue;
        }
        if (argument == "-V" || argument == "--version") {
            parsed.versionRequested = true;
            ++index;
            continue;
        }
        break;
    }

    if (index < arguments.size() && !strings::startsWith(arguments[index], "-")) {
        parsed.command = strings::toLower(arguments[index++]);
    }

    // `ai generate-tests`, `db prune`: a second bare word is a subcommand.
    //
    // `spec` is deliberately not in this list. Its second word is a file
    // path, and treating it as a subcommand both hid it from the command
    // (which reads positionals) and lower-cased it, quietly breaking any
    // path with a capital letter in it. Commands::spec skips an optional
    // leading "load" verb itself.
    if ((parsed.command == "ai" || parsed.command == "db") && index < arguments.size() &&
        !strings::startsWith(arguments[index], "-")) {
        parsed.subcommand = strings::toLower(arguments[index++]);
    }

    const std::vector<std::string>& commandOptions = knownOptions(parsed.command);

    for (; index < arguments.size(); ++index) {
        std::string argument = arguments[index];

        if (argument == "--") {
            // Everything after "--" is positional, by convention.
            for (++index; index < arguments.size(); ++index) {
                parsed.positionals.push_back(arguments[index]);
            }
            break;
        }

        if (!strings::startsWith(argument, "-")) {
            parsed.positionals.push_back(std::move(argument));
            continue;
        }

        if (argument == "-h") {
            parsed.helpRequested = true;
            continue;
        }
        if (argument == "-V") {
            parsed.versionRequested = true;
            continue;
        }
        if (!strings::startsWith(argument, "--")) {
            parsed.error = "unknown short option '" + argument +
                           "' (only -h and -V are supported; use the long form otherwise)";
            return parsed;
        }

        std::string name = argument.substr(2);
        std::optional<std::string> inlineValue;
        if (const std::size_t equals = name.find('='); equals != std::string::npos) {
            inlineValue = name.substr(equals + 1);
            name = name.substr(0, equals);
        }

        if (name == "help") {
            parsed.helpRequested = true;
            continue;
        }
        if (name == "version") {
            parsed.versionRequested = true;
            continue;
        }

        const bool known =
            std::find(commandOptions.begin(), commandOptions.end(), name) != commandOptions.end() ||
            std::find(globalOptions().begin(), globalOptions().end(), name) !=
                globalOptions().end();
        if (!known) {
            // Refusing an unrecognised option is the whole point: a typo that
            // silently widened a filter would be a very expensive kind of bug.
            std::string message = "unknown option '--" + name + "'";
            if (!parsed.command.empty()) {
                message += " for command '" + parsed.command + "'";
            }
            message += "\nRun 'testforge " + parsed.command + " --help' to see the options.";
            parsed.error = message;
            return parsed;
        }

        if (inlineValue.has_value()) {
            parsed.options[name].push_back(*inlineValue);
            continue;
        }
        if (isFlagOption(name)) {
            parsed.options[name];  // present with no value
            continue;
        }
        if (index + 1 >= arguments.size() || strings::startsWith(arguments[index + 1], "--")) {
            parsed.error = "option '--" + name + "' requires a value";
            return parsed;
        }
        parsed.options[name].push_back(arguments[++index]);
    }

    if (parsed.command.empty() && !parsed.helpRequested && !parsed.versionRequested) {
        parsed.helpRequested = true;
    }

    if (!parsed.command.empty() && !parsed.helpRequested && !parsed.versionRequested) {
        const std::vector<std::string>& all = commands();
        if (std::find(all.begin(), all.end(), parsed.command) == all.end()) {
            parsed.error = "unknown command '" + parsed.command + "'\nRun 'testforge help'.";
        }
    }

    return parsed;
}

TestFilter CommandLine::filterFrom(const ParsedCommand& parsed) {
    TestFilter filter;
    filter.suites = parsed.values("suite");
    filter.names = parsed.values("test");
    filter.tags = parsed.values("tag");
    filter.excludeTags = parsed.values("exclude-tag");
    filter.excludeSuites = parsed.values("exclude-suite");
    filter.includeDisabled = parsed.flag("include-disabled");

    // Bare positionals after `run` are treated as test-name patterns, so
    // `testforge run api.health_check` works without remembering --test.
    for (const std::string& positional : parsed.positionals) {
        filter.names.push_back(positional);
    }
    return filter;
}

SelectionOptions CommandLine::selectionFrom(const ParsedCommand& parsed) {
    SelectionOptions options;
    options.shuffle = parsed.flag("shuffle");
    options.seed = static_cast<std::uint64_t>(parsed.int64Value("seed", 0));
    options.shardCount = std::max(1, parsed.intValue("shards", 1));
    options.shardIndex = std::max(0, parsed.intValue("shard", 0));
    return options;
}

}  // namespace testforge::cli
