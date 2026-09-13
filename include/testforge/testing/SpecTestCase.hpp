#pragma once

#include "testforge/ai/TestSpec.hpp"
#include "testforge/core/Config.hpp"
#include "testforge/core/TestCase.hpp"

#include <cstddef>
#include <string>

namespace testforge {

class TestRegistry;

/// Executes a declarative TestSpec.
///
/// This is the only thing that ever runs generated content, and it is a plain
/// interpreter over a fixed set of fields: it issues one HTTP request and
/// evaluates a closed list of assertion kinds. There is no eval, no code
/// generation, no shell, no dynamic loading. A malicious specification cannot
/// do anything a well-behaved one could not, because every field it can set is
/// one this class already knows how to handle.
///
/// The specification must have passed SpecValidator first — the registration
/// helper below enforces that.
class SpecTestCase final : public TestCase {
 public:
    SpecTestCase(TestMetadata metadata, ai::TestSpec spec);

    void execute(TestContext& context) override;

    [[nodiscard]] const TestMetadata& metadata() const override { return metadata_; }

    [[nodiscard]] const ai::TestSpec& spec() const noexcept { return spec_; }

 private:
    TestMetadata metadata_;
    ai::TestSpec spec_;
};

/// Registers every test in a validated suite, replacing any previous
/// registration for the same suite.
///
/// Returns how many tests were registered. Throws SecurityError if the suite
/// still contains anything the validator would reject — a second line of
/// defence against a caller that forgot to validate.
std::size_t registerSpecSuite(TestRegistry& registry,
                              const ai::TestSpecSuite& suite,
                              const Config& config);

}  // namespace testforge
