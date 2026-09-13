#include "testforge/core/Ids.hpp"

#include "testforge/core/StringUtils.hpp"

#include <array>
#include <cstdint>
#include <random>
#include <sstream>

namespace testforge::ids {
namespace {

/// One generator per thread: std::mt19937_64 is not thread-safe, and a shared
/// one behind a mutex would serialise every id allocation on the hot path.
std::mt19937_64& threadLocalEngine() {
    static thread_local std::mt19937_64 engine([] {
        std::random_device device;
        // random_device may be a fixed sequence on some platforms; mixing in
        // the address of a stack object and the thread id keeps ids distinct
        // between threads even in that case.
        const std::uint64_t seed = (static_cast<std::uint64_t>(device()) << 32u) ^
                                   static_cast<std::uint64_t>(device()) ^
                                   reinterpret_cast<std::uintptr_t>(&device);
        return std::mt19937_64(seed);
    }());
    return engine;
}

}  // namespace

std::uint64_t fnv1a64(std::string_view text) noexcept {
    constexpr std::uint64_t kOffsetBasis = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;

    std::uint64_t hash = kOffsetBasis;
    for (const char c : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= kPrime;
    }
    return hash;
}

std::string generateHexId(std::size_t bytes) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::uniform_int_distribution<std::uint32_t> dist(0, 15);
    auto& engine = threadLocalEngine();

    std::string out;
    out.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes * 2; ++i) {
        out.push_back(kDigits[dist(engine)]);
    }
    return out;
}

std::string testId(std::string_view suite, std::string_view name) {
    std::string composite;
    composite.reserve(suite.size() + name.size() + 1);
    composite.append(suite);
    composite.push_back('.');
    composite.append(name);

    const std::uint64_t hash = fnv1a64(composite);

    std::array<char, 17> buffer{};
    static constexpr std::string_view kDigits = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        buffer[static_cast<std::size_t>(i)] =
            kDigits[(hash >> (4u * static_cast<unsigned>(15 - i))) & 0xFu];
    }
    return {buffer.data(), 16};
}

}  // namespace testforge::ids
