#pragma once

#include <cstdint>

namespace MagicSpatial {

// Detects transients using dual envelope followers (fast and slow).
// Output is a per-sample transient intensity in [0, 1].
class TransientDetector {
public:
    void Initialize(float sampleRate);

    // Process mono signal, writing per-sample transient values to output.
    void Process(const float* input, float* output, uint32_t frameCount);

    void Reset();

private:
    float m_fastAttack  = 0.0f;  // ~0.5ms
    float m_fastRelease = 0.0f;  // ~5ms
    float m_slowAttack  = 0.0f;  // ~20ms
    float m_slowRelease = 0.0f;  // ~100ms

    float m_fastEnv = 0.0f;
    float m_slowEnv = 0.0f;
};

} // namespace MagicSpatial
