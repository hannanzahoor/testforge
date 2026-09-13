#pragma once

#include <string>
#include <string_view>

namespace testforge {

/// Identifier generation for runs, results and events.
///
/// These are correlation identifiers, not security tokens: they are seeded
/// from std::random_device but no cryptographic strength is claimed or
/// required.
namespace ids {

/// 32 lowercase hex characters, e.g. "9f2c1a...". Used for run identifiers.
std::string generateHexId(std::size_t bytes = 16);

/// Stable identifier for a test, derived from suite + name.
///
/// Deterministic on purpose: history and flake analysis need the same test to
/// carry the same id across runs and across machines.
std::string testId(std::string_view suite, std::string_view name);

/// FNV-1a, 64-bit. Small, dependency-free and adequate for id derivation.
std::uint64_t fnv1a64(std::string_view text) noexcept;

}  // namespace ids
}  // namespace testforge
