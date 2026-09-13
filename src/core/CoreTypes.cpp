// Small out-of-line definitions for the core value types: Version,
// SourceLocation and AssertionFailure. Kept together because each is a handful
// of lines and a file per function would be noise.

#include "testforge/core/Exceptions.hpp"
#include "testforge/core/SourceLocation.hpp"
#include "testforge/core/Version.hpp"

#include <sstream>
#include <string>

namespace testforge {
namespace {

/// Strips directories so reports do not embed the absolute path of the build
/// machine (which is both noise and a minor information leak).
std::string basename(const char* path) {
    if (path == nullptr) {
        return "";
    }
    const std::string text(path);
    const std::size_t slash = text.find_last_of("/\\");
    return slash == std::string::npos ? text : text.substr(slash + 1);
}

}  // namespace

std::string Version::string() {
    std::ostringstream os;
    os << kMajor << '.' << kMinor << '.' << kPatch;
    return os.str();
}

std::string Version::banner() {
    std::ostringstream os;
    os << "TestForge " << string() << " (C++" << (__cplusplus / 100 % 100) << ", ";
#if defined(__clang__)
    os << "clang " << __clang_major__ << '.' << __clang_minor__ << '.' << __clang_patchlevel__;
#elif defined(__GNUC__)
    os << "gcc " << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
#elif defined(_MSC_VER)
    os << "msvc " << _MSC_VER;
#else
    os << "unknown compiler";
#endif
    os << ')';
    return os.str();
}

std::string SourceLocation::brief() const {
    if (!valid()) {
        return "<unknown>";
    }
    return basename(file) + ":" + std::to_string(line);
}

std::string SourceLocation::full() const {
    if (!valid()) {
        return "<unknown>";
    }
    std::ostringstream os;
    os << basename(file) << ':' << line;
    if (function != nullptr && *function != '\0') {
        os << " in " << function << "()";
    }
    return os.str();
}

std::string AssertionFailure::detail() const {
    std::ostringstream os;
    os << what();
    if (!expression_.empty()) {
        os << "\n  Expression: " << expression_;
    }
    // Expected/actual are printed even when empty-looking, because "" vs "  "
    // is exactly the kind of difference a developer needs to see.
    if (!expected_.empty() || !actual_.empty()) {
        os << "\n  Expected:   " << expected_;
        os << "\n  Actual:     " << actual_;
    }
    if (where_.valid()) {
        os << "\n  At:         " << where_.full();
    }
    return os.str();
}

}  // namespace testforge
