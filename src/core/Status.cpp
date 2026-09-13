#include "testforge/core/Status.hpp"

#include "testforge/core/StringUtils.hpp"

namespace testforge {

std::string_view toString(TestStatus status) noexcept {
    switch (status) {
        case TestStatus::Passed:
            return "PASSED";
        case TestStatus::Failed:
            return "FAILED";
        case TestStatus::Skipped:
            return "SKIPPED";
        case TestStatus::Error:
            return "ERROR";
        case TestStatus::Timeout:
            return "TIMEOUT";
    }
    return "ERROR";
}

std::string_view toString(FailureCategory category) noexcept {
    switch (category) {
        case FailureCategory::None:
            return "NONE";
        case FailureCategory::AssertionFailure:
            return "ASSERTION_FAILURE";
        case FailureCategory::Timeout:
            return "TIMEOUT";
        case FailureCategory::NetworkFailure:
            return "NETWORK_FAILURE";
        case FailureCategory::EnvironmentFailure:
            return "ENVIRONMENT_FAILURE";
        case FailureCategory::ResourceFailure:
            return "RESOURCE_FAILURE";
        case FailureCategory::ApplicationFailure:
            return "APPLICATION_FAILURE";
        case FailureCategory::DependencyFailure:
            return "DEPENDENCY_FAILURE";
        case FailureCategory::ConfigurationFailure:
            return "CONFIGURATION_FAILURE";
        case FailureCategory::FrameworkError:
            return "FRAMEWORK_ERROR";
        case FailureCategory::Unknown:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::optional<TestStatus> testStatusFromString(std::string_view text) noexcept {
    for (const TestStatus status : allTestStatuses()) {
        if (strings::equalsIgnoreCase(text, toString(status))) {
            return status;
        }
    }
    return std::nullopt;
}

std::optional<FailureCategory> failureCategoryFromString(std::string_view text) noexcept {
    for (const FailureCategory category : allFailureCategories()) {
        if (strings::equalsIgnoreCase(text, toString(category))) {
            return category;
        }
    }
    return std::nullopt;
}

const std::vector<TestStatus>& allTestStatuses() {
    static const std::vector<TestStatus> kAll = {TestStatus::Passed,
                                                 TestStatus::Failed,
                                                 TestStatus::Skipped,
                                                 TestStatus::Error,
                                                 TestStatus::Timeout};
    return kAll;
}

const std::vector<FailureCategory>& allFailureCategories() {
    static const std::vector<FailureCategory> kAll = {FailureCategory::None,
                                                      FailureCategory::AssertionFailure,
                                                      FailureCategory::Timeout,
                                                      FailureCategory::NetworkFailure,
                                                      FailureCategory::EnvironmentFailure,
                                                      FailureCategory::ResourceFailure,
                                                      FailureCategory::ApplicationFailure,
                                                      FailureCategory::DependencyFailure,
                                                      FailureCategory::ConfigurationFailure,
                                                      FailureCategory::FrameworkError,
                                                      FailureCategory::Unknown};
    return kAll;
}

std::string_view ansiColor(TestStatus status) noexcept {
    switch (status) {
        case TestStatus::Passed:
            return "\033[32m";  // green
        case TestStatus::Failed:
            return "\033[31m";  // red
        case TestStatus::Skipped:
            return "\033[90m";  // grey
        case TestStatus::Error:
            return "\033[35m";  // magenta
        case TestStatus::Timeout:
            return "\033[33m";  // yellow
    }
    return "";
}

}  // namespace testforge
