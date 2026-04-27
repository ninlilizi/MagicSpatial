#pragma once

#include <cstdint>
#include <vector>

namespace MagicSpatial {

// Minimal 4-tap feedback delay network (FDN) for diffuse reverberant tails.
// Applied in-place after an early-reflection pre-delay to turn a discrete
// echo into a smooth wash of reflections. Suitable for rear/overhead objects
// where "room feel" matters more than transient fidelity.
//
// Architecture:
//   - 4 delay lines at prime-ish staggered lengths (~3/5/7/11 ms)
//   - Hadamard-style mixing matrix couples the taps each iteration
//   - 1-pole lowpass damping on each feedback path (natural HF decay)
//   - Configurable feedback coefficient (~0.3) and wet mix (~0.4)
class FeedbackDiffuser {
public:
    static constexpr int kNumTaps = 4;

    void Initialize(float sampleRate);
    void Process(float* buffer, uint32_t frameCount);  // in-place
    void Reset();

private:
    struct DelayTap {
        std::vector<float> ring;
        int length = 0;
        int pos = 0;
        float dampState = 0.0f;
    };
    DelayTap m_taps[kNumTaps];

    float m_feedback = 0.26f;
    float m_wetMix   = 0.22f;
    float m_dampCoeff = 0.0f;   // 1-pole LP coefficient (shared across taps)
};

} // namespace MagicSpatial
