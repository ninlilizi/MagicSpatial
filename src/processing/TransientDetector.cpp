#include "processing/TransientDetector.h"
#include <cmath>
#include <algorithm>

namespace MagicSpatial {

void TransientDetector::Initialize(float sampleRate) {
    // Single-pole coefficient: coeff = 1 - exp(-1 / (timeSeconds * sampleRate))
    m_fastAttack  = 1.0f - std::exp(-1.0f / (0.0005f * sampleRate));  // 0.5ms
    m_fastRelease = 1.0f - std::exp(-1.0f / (0.005f  * sampleRate));  // 5ms
    m_slowAttack  = 1.0f - std::exp(-1.0f / (0.020f  * sampleRate));  // 20ms
    m_slowRelease = 1.0f - std::exp(-1.0f / (0.100f  * sampleRate));  // 100ms

    m_fastEnv = 0.0f;
    m_slowEnv = 0.0f;
}

void TransientDetector::Process(const float* input, float* output, uint32_t frameCount) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        float absVal = std::fabs(input[i]);

        // Fast envelope follower
        float fastCoeff = (absVal > m_fastEnv) ? m_fastAttack : m_fastRelease;
        m_fastEnv += fastCoeff * (absVal - m_fastEnv);

        // Slow envelope follower
        float slowCoeff = (absVal > m_slowEnv) ? m_slowAttack : m_slowRelease;
        m_slowEnv += slowCoeff * (absVal - m_slowEnv);

        // Transient ratio: how much fast exceeds slow
        float ratio = m_fastEnv / (m_slowEnv + 1e-10f) - 1.0f;
        output[i] = std::clamp(ratio, 0.0f, 1.0f);
    }
}

void TransientDetector::Reset() {
    m_fastEnv = 0.0f;
    m_slowEnv = 0.0f;
}

} // namespace MagicSpatial
