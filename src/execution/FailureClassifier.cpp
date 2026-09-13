#include "testforge/execution/FailureClassifier.hpp"

#include "testforge/core/Exceptions.hpp"
#include "testforge/core/StringUtils.hpp"

#include <array>
#include <utility>

namespace testforge {
namespace {

/// Substrings that reliably indicate a particular kind of failure. Order
/// matters: the first match wins, so the more specific phrases come first.
constexpr std::array<std::pair<std::string_view, FailureCategory>, 22> kMessageSignals = {{
    {"connection refused", FailureCategory::NetworkFailure},
    {"connection reset", FailureCategory::NetworkFailure},
    {"could not resolve", FailureCategory::NetworkFailure},
    {"name or service not known", FailureCategory::NetworkFailure},
    {"no route to host", FailureCategory::NetworkFailure},
    {"network is unreachable", FailureCategory::NetworkFailure},
    {"broken pipe", FailureCategory::NetworkFailure},
    {"timed out", FailureCategory::Timeout},
    {"timeout", FailureCategory::Timeout},
    {"deadline exceeded", FailureCategory::Timeout},
    {"no such file or directory", FailureCategory::DependencyFailure},
    {"executable not found", FailureCategory::DependencyFailure},
    {"command not found", FailureCategory::DependencyFailure},
    {"not installed", FailureCategory::DependencyFailure},
    {"permission denied", FailureCategory::EnvironmentFailure},
    {"read-only file system", FailureCategory::EnvironmentFailure},
    {"cannot allocate memory", FailureCategory::ResourceFailure},
    {"out of memory", FailureCategory::ResourceFailure},
    {"no space left", FailureCategory::ResourceFailure},
    {"too many open files", FailureCategory::ResourceFailure},
    {"internal server error", FailureCategory::ApplicationFailure},
    {"bad gateway", FailureCategory::ApplicationFailure},
}};

/// Ranks categories by how specific they are, so refine() never replaces a
/// precise diagnosis with a vaguer one.
int specificity(FailureCategory category) {
    switch (category) {
        case FailureCategory::None:
            return 0;
        case FailureCategory::Unknown:
            return 1;
        case FailureCategory::AssertionFailure:
            return 2;
        case FailureCategory::FrameworkError:
        case FailureCategory::ConfigurationFailure:
        case FailureCategory::Timeout:
            return 4;
        case FailureCategory::ApplicationFailure:
        case FailureCategory::NetworkFailure:
        case FailureCategory::DependencyFailure:
        case FailureCategory::EnvironmentFailure:
        case FailureCategory::ResourceFailure:
            return 3;
    }
    return 1;
}

}  // namespace

FailureCategory FailureClassifier::fromMessage(std::string_view message) {
    const std::string lowered = strings::toLower(message);
    for (const auto& [needle, category] : kMessageSignals) {
        if (lowered.find(needle) != std::string::npos) {
            return category;
        }
    }
    return FailureCategory::Unknown;
}

FailureClassifier::Classification FailureClassifier::fromException(const std::exception& error) {
    Classification classification;

    // The message is a single line: it goes in the console's result table and
    // in a database column that reports sort by. Anything past the first line
    // is detail, and is preserved as such below.
    const std::string full = error.what();
    const std::size_t firstBreak = full.find('\n');
    classification.message = firstBreak == std::string::npos ? full : full.substr(0, firstBreak);
    if (firstBreak != std::string::npos) {
        classification.detail = full;
    }

    // Skip is not a failure at all.
    if (dynamic_cast<const SkipTest*>(&error) != nullptr) {
        classification.status = TestStatus::Skipped;
        classification.category = FailureCategory::None;
        return classification;
    }

    if (const auto* cancelled = dynamic_cast<const TestCancelled*>(&error); cancelled != nullptr) {
        classification.status = TestStatus::Timeout;
        classification.category = FailureCategory::Timeout;
        return classification;
    }

    if (const auto* assertion = dynamic_cast<const AssertionFailure*>(&error);
        assertion != nullptr) {
        classification.status = TestStatus::Failed;
        classification.category = FailureCategory::AssertionFailure;
        classification.detail = assertion->detail();
        return classification;
    }

    if (const auto* forge = dynamic_cast<const TestForgeError*>(&error); forge != nullptr) {
        classification.category = forge->category();
        // Everything that is not an assertion is an *error*: the test could not
        // render a verdict, which is materially different from a verdict of
        // "the system under test is wrong".
        classification.status = classification.category == FailureCategory::Timeout
                                    ? TestStatus::Timeout
                                    : TestStatus::Error;
        return classification;
    }

    // A standard-library or third-party exception. All we have is the text.
    classification.status = TestStatus::Error;
    classification.category = fromMessage(classification.message);
    if (classification.category == FailureCategory::Unknown) {
        // std::bad_alloc is the one std exception whose type is diagnostic.
        if (dynamic_cast<const std::bad_alloc*>(&error) != nullptr) {
            classification.category = FailureCategory::ResourceFailure;
        }
    }
    // Append rather than assign: the detail may already hold the full
    // multi-line message captured above, and the exception type is extra
    // context, not a replacement for it.
    if (!classification.detail.empty()) {
        classification.detail += "\n";
    }
    classification.detail += std::string("exception type: ") + typeid(error).name();
    return classification;
}

FailureClassifier::Classification FailureClassifier::fromUnknownException() {
    Classification classification;
    classification.status = TestStatus::Error;
    classification.category = FailureCategory::Unknown;
    classification.message = "test threw a non-std::exception object";
    classification.detail =
        "TestForge caught an exception that does not derive from std::exception, so no "
        "diagnostic information could be extracted. Throw a std::exception subclass "
        "(or use the TF_ASSERT_* macros) to get a useful failure report.";
    return classification;
}

FailureCategory FailureClassifier::refine(FailureCategory base, const json::Value& metadata) {
    FailureCategory refined = base;

    auto promote = [&refined](FailureCategory candidate) {
        if (specificity(candidate) > specificity(refined)) {
            refined = candidate;
        }
    };

    if (const json::Value* status = metadata.find("http_status");
        status != nullptr && status->isNumber()) {
        const std::int64_t code = status->asInt();
        if (code >= 500) {
            // The service answered, and answered badly: that is the
            // application's fault, not the test's.
            promote(FailureCategory::ApplicationFailure);
        }
    }

    if (const json::Value* transport = metadata.find("transport_error");
        transport != nullptr && transport->isString()) {
        promote(fromMessage(transport->asString()));
    }

    if (const json::Value* missing = metadata.find("missing_dependency");
        missing != nullptr && missing->isString() && !missing->asString().empty()) {
        promote(FailureCategory::DependencyFailure);
    }

    if (refined == FailureCategory::Unknown) {
        if (const json::Value* note = metadata.find("error_hint");
            note != nullptr && note->isString()) {
            promote(fromMessage(note->asString()));
        }
    }

    return refined;
}

std::string_view FailureClassifier::explain(FailureCategory category) {
    switch (category) {
        case FailureCategory::None:
            return "No failure.";
        case FailureCategory::AssertionFailure:
            return "The system under test produced the wrong result. Investigate the SUT, or "
                   "the expectation if the behaviour changed on purpose.";
        case FailureCategory::Timeout:
            return "The test did not finish within its deadline. Look for a hung dependency, a "
                   "deadlock, or a budget that is simply too tight.";
        case FailureCategory::NetworkFailure:
            return "The system under test could not be reached. Usually the service is not "
                   "running, or the address or port is wrong.";
        case FailureCategory::EnvironmentFailure:
            return "The machine running the test is at fault: permissions, missing paths, or a "
                   "misconfigured host.";
        case FailureCategory::ResourceFailure:
            return "The machine ran out of something: memory, disk, or file descriptors.";
        case FailureCategory::ApplicationFailure:
            return "The system under test errored internally (5xx or a crash). Check its logs "
                   "rather than the test.";
        case FailureCategory::DependencyFailure:
            return "A required binary or service is absent. Install it, or let the test skip.";
        case FailureCategory::ConfigurationFailure:
            return "TestForge was configured incorrectly. Fix the config file, flags, or "
                   "environment variables.";
        case FailureCategory::FrameworkError:
            return "A defect in TestForge itself. This should be reported as a bug.";
        case FailureCategory::Unknown:
            return "Could not be classified automatically. Read the logs and diagnostics.";
    }
    return "Unclassified.";
}

bool FailureClassifier::isRetryable(FailureCategory category) {
    switch (category) {
        case FailureCategory::NetworkFailure:
        case FailureCategory::Timeout:
        case FailureCategory::ResourceFailure:
            // Genuinely transient classes of problem.
            return true;
        case FailureCategory::AssertionFailure:
        case FailureCategory::ApplicationFailure:
        case FailureCategory::ConfigurationFailure:
        case FailureCategory::DependencyFailure:
        case FailureCategory::EnvironmentFailure:
        case FailureCategory::FrameworkError:
        case FailureCategory::None:
        case FailureCategory::Unknown:
            return false;
    }
    return false;
}

}  // namespace testforge
