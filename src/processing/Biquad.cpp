#include "processing/Biquad.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace MagicSpatial {

BiquadCoeffs DesignLowpass(float cutoffHz, float sampleRate) {
    const float w0 = 2.0f * static_cast<float>(M_PI) * cutoffHz / sampleRate;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float alpha = sinw0 / (2.0f * 0.7071067811865476f); // Q = sqrt(2)/2 for Butterworth

    const float a0 = 1.0f + alpha;

    BiquadCoeffs c;
    c.b0 = ((1.0f - cosw0) / 2.0f) / a0;
    c.b1 = (1.0f - cosw0) / a0;
    c.b2 = c.b0;
    c.a1 = (-2.0f * cosw0) / a0;
    c.a2 = (1.0f - alpha) / a0;
    return c;
}

BiquadCoeffs DesignHighpass(float cutoffHz, float sampleRate) {
    const float w0 = 2.0f * static_cast<float>(M_PI) * cutoffHz / sampleRate;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float alpha = sinw0 / (2.0f * 0.7071067811865476f);

    const float a0 = 1.0f + alpha;

    BiquadCoeffs c;
    c.b0 = ((1.0f + cosw0) / 2.0f) / a0;
    c.b1 = (-(1.0f + cosw0)) / a0;
    c.b2 = c.b0;
    c.a1 = (-2.0f * cosw0) / a0;
    c.a2 = (1.0f - alpha) / a0;
    return c;
}

void ProcessBiquad(const BiquadCoeffs& c, BiquadState& s,
                   const float* input, float* output, uint32_t count) {
    // Direct Form II Transposed
    for (uint32_t i = 0; i < count; ++i) {
        const float x = input[i];
        const float y = c.b0 * x + s.z1;
        s.z1 = c.b1 * x - c.a1 * y + s.z2;
        s.z2 = c.b2 * x - c.a2 * y;
        output[i] = y;
    }
}

void BiquadFilter::Process(const float* input, float* output, uint32_t count) {
    // Direct Form II Transposed for numerical stability
    for (uint32_t i = 0; i < count; ++i) {
        const float x = input[i];
        const float y = m_coeffs.b0 * x + m_state.z1;
        m_state.z1 = m_coeffs.b1 * x - m_coeffs.a1 * y + m_state.z2;
        m_state.z2 = m_coeffs.b2 * x - m_coeffs.a2 * y;
        output[i] = y;
    }
}

} // namespace MagicSpatial
