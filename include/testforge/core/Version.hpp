#pragma once

#include <string>

namespace testforge {

struct Version {
    static constexpr int kMajor = 0;
    static constexpr int kMinor = 9;
    static constexpr int kPatch = 0;

    static std::string string();

    /// Human readable build banner, e.g. "TestForge 0.9.0 (C++20, GNU 11.4.0)".
    static std::string banner();
};

}  // namespace testforge
