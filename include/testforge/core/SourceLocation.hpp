#pragma once

#include <cstdint>
#include <string>

namespace testforge {

/// Minimal stand-in for std::source_location.
///
/// A hand-rolled struct is used instead of <source_location> because every
/// capture point is already a macro (the assertion macros), and this keeps the
/// public headers compilable on toolchains with incomplete C++20 library
/// support.
struct SourceLocation {
    const char* file = "";
    std::uint32_t line = 0;
    const char* function = "";

    [[nodiscard]] bool valid() const noexcept { return line != 0; }

    /// "Assertions.cpp:42" — basename only, so reports never leak the absolute
    /// build path of the machine that produced them.
    [[nodiscard]] std::string brief() const;

    [[nodiscard]] std::string full() const;
};

#define TESTFORGE_CURRENT_LOCATION()                             \
    ::testforge::SourceLocation {                                \
        __FILE__, static_cast<std::uint32_t>(__LINE__), __func__ \
    }

}  // namespace testforge
