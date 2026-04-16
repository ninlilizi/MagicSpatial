#include "processing/FeedbackDiffuser.h"

#include <cmath>
#include <cstring>

namespace MagicSpatial {

namespace {
    // Staggered delay times in milliseconds — prime-ish spacing avoids
    // comb-filter patterns that would colour the output tonally.
    constexpr float kTapDelaysMs[FeedbackDiffuser::kNumTaps] = {
        3.0f, 5.0f, 7.0f, 11.0f
    };

    // Damping cutoff — 1-pole lowpass target frequency.  Each feedback
    // iteration loses energy above this, simulating air/wall absorption.
    constexpr float kDampCutoffHz = 6000.0f;
}

void FeedbackDiffuser::Initialize(float sampleRate) {
    // 1-pole LP coefficient: α = exp(-2π·fc/fs)
    m_dampCoeff = std::exp(-2.0f * 3.14159265f * kDampCutoffHz / sampleRate);

    for (int t = 0; t < kNumTaps; ++t) {
        int len = static_cast<int>(kTapDelaysMs[t] * sampleRate / 1000.0f + 0.5f);
        if (len < 1) len = 1;
        m_taps[t].length = len;
        m_taps[t].ring.assign(len, 0.0f);
        m_taps[t].pos = 0;
        m_taps[t].dampState = 0.0f;
    }
}

void FeedbackDiffuser::Reset() {
    for (int t = 0; t < kNumTaps; ++t) {
        std::fill(m_taps[t].ring.begin(), m_taps[t].ring.end(), 0.0f);
        m_taps[t].pos = 0;
        m_taps[t].dampState = 0.0f;
    }
}

void FeedbackDiffuser::Process(float* buffer, uint32_t frameCount) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        float dry = buffer[i];

        // Read from each tap
        float tapOut[kNumTaps];
        for (int t = 0; t < kNumTaps; ++t) {
            tapOut[t] = m_taps[t].ring[m_taps[t].pos];
        }

        // Hadamard-style 4×4 mixing (scaled by 0.5 for energy conservation):
        //   [+1 +1 +1 +1]        [a+b+c+d]
        //   [+1 -1 +1 -1]   →    [a-b+c-d]   × 0.5
        //   [+1 +1 -1 -1]        [a+b-c-d]
        //   [+1 -1 -1 +1]        [a-b-c+d]
        float a = tapOut[0], b = tapOut[1], c = tapOut[2], d = tapOut[3];
        float mixed[kNumTaps];
        mixed[0] = (a + b + c + d) * 0.5f;
        mixed[1] = (a - b + c - d) * 0.5f;
        mixed[2] = (a + b - c - d) * 0.5f;
        mixed[3] = (a - b - c + d) * 0.5f;

        // Feedback: damp + scale, then write back into delay lines
        for (int t = 0; t < kNumTaps; ++t) {
            // 1-pole lowpass damping
            m_taps[t].dampState = m_dampCoeff * m_taps[t].dampState
                                + (1.0f - m_dampCoeff) * mixed[t];
            float fb = m_taps[t].dampState * m_feedback;

            // Write input + feedback into the delay line
            m_taps[t].ring[m_taps[t].pos] = dry + fb;
            m_taps[t].pos = (m_taps[t].pos + 1) % m_taps[t].length;
        }

        // Output: dry + wet mix of tap sum
        float wetSum = (tapOut[0] + tapOut[1] + tapOut[2] + tapOut[3]) * 0.25f;
        buffer[i] = dry + wetSum * m_wetMix;
    }
}

} // namespace MagicSpatial
