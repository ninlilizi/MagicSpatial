#pragma once

#include <cstdint>

namespace MagicSpatial {

struct BiquadCoeffs {
    float b0 = 0, b1 = 0, b2 = 0;
    float a1 = 0, a2 = 0;
    // Note: a0 is normalized to 1.0
};

struct BiquadState {
    float z1 = 0, z2 = 0;
};

// Design a 2nd-order Butterworth lowpass filter.
BiquadCoeffs DesignLowpass(float cutoffHz, float sampleRate);

// Design a 2nd-order Butterworth highpass filter.
BiquadCoeffs DesignHighpass(float cutoffHz, float sampleRate);

// Process a block of samples through the biquad in-place or out-of-place.
void ProcessBiquad(const BiquadCoeffs& coeffs, BiquadState& state,
                   const float* input, float* output, uint32_t count);

// A self-contained biquad filter instance.
class BiquadFilter {
public:
    BiquadFilter() = default;
    explicit BiquadFilter(const BiquadCoeffs& coeffs) : m_coeffs(coeffs) {}

    void SetCoeffs(const BiquadCoeffs& coeffs) { m_coeffs = coeffs; }
    void Process(const float* input, float* output, uint32_t count);
    void Reset() { m_state = {}; }

private:
    BiquadCoeffs m_coeffs;
    BiquadState m_state;
};

} // namespace MagicSpatial
