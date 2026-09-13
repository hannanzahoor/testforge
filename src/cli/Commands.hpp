#pragma once

#include "CommandLine.hpp"

#include "testforge/api/TestForgeService.hpp"

#include <string>

namespace testforge::cli {

/// Exit codes. CI depends on the difference between "tests failed" and
/// "the tool could not run", so they are distinct and documented.
namespace exitcode {
inline constexpr int kSuccess = 0;         ///< everything passed
inline constexpr int kTestsFailed = 1;     ///< the run completed, some tests failed
inline constexpr int kUsageError = 2;      ///< bad arguments or bad configuration
inline constexpr int kFrameworkError = 3;  ///< TestForge itself failed
}  // namespace exitcode

/// The command handlers. Each returns a process exit code.
class Commands {
 public:
    static int list(api::TestForgeService& service, const ParsedCommand& parsed);
    static int run(api::TestForgeService& service, const ParsedCommand& parsed);
    static int report(api::TestForgeService& service, const ParsedCommand& parsed);
    static int diagnose(api::TestForgeService& service, const ParsedCommand& parsed);
    static int history(api::TestForgeService& service, const ParsedCommand& parsed);
    static int stats(api::TestForgeService& service, const ParsedCommand& parsed);
    static int serve(api::TestForgeService& service, const ParsedCommand& parsed);
    static int ai(api::TestForgeService& service, const ParsedCommand& parsed);
    static int spec(api::TestForgeService& service, const ParsedCommand& parsed);
    static int database(api::TestForgeService& service, const ParsedCommand& parsed);
    static int version(const ParsedCommand& parsed);

    /// Prints general help, or help for one command.
    static int help(const std::string& command);
};

}  // namespace testforge::cli
