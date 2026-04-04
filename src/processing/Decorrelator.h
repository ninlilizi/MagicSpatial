#pragma once

#include <cstdint>
#include <vector>
#include <initializer_list>

namespace MagicSpatial {

// Allpass filter cascade with a pre-delay for audio decorrelation.
// Each instance uses different coefficients to produce a perceptually
// distinct output from the same source signal.
class Decorrelator {
public:
    Decorrelator() = default;

    // Initialize with allpass coefficients and a pre-delay in samples.
    Decorrelator(std::initializer_list<float> coefficients, size_t delaySamples);

    void Initialize(const float* coefficients, size_t numCoeffs, size_t delaySamples);

    void Process(const float* input, float* output, uint32_t frameCount);
    void Reset();

private:
    struct AllpassSection {
        float coeff = 0;
        float state = 0;
    };

    std::vector<AllpassSection> m_sections;
    std::vector<float> m_delayLine;
    size_t m_delayLength = 0;
    size_t m_writeIndex = 0;
};

// Pre-defined decorrelator configurations.
// 8 distinct configurations for up to 8 decorrelated outputs.
struct DecorrelatorConfig {
    float coefficients[3];
    size_t delaySamples;
};

// Returns 8 preset configurations with distinct allpass/delay parameters.
const DecorrelatorConfig* GetDecorrelatorPresets();

} // namespace MagicSpatial
