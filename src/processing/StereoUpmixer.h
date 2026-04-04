#pragma once

#include "processing/Biquad.h"
#include "processing/Decorrelator.h"
#include "core/Types.h"

#include <cstdint>
#include <vector>

namespace MagicSpatial {

// Upmixes stereo (2.0) to 7.1.4 Atmos bed.
//
// Strategy:
//   - Center/LFE: Mid/Side decomposition, lowpass-filtered center and LFE
//   - Surrounds: Decorrelated Side signal for SL/SR/BL/BR
//   - Heights: Highpass-filtered front/surround through decorrelators
class StereoUpmixer {
public:
    void Initialize(uint32_t sampleRate, uint32_t maxFrameCount);

    // Input: 2 separate channel pointers (L, R).
    // Output: 12 mono channel pointers, each frameCount samples.
    void Process(const float* const* input, uint32_t frameCount, float** output);

    void Reset();

private:
    BiquadFilter m_centerLowpass;     // ~500 Hz for center extraction
    BiquadFilter m_lfeLowpass;        // ~80 Hz for LFE
    BiquadFilter m_heightHighpass[4]; // ~3 kHz for height channels

    Decorrelator m_decorrelators[8];

    // Scratch buffers for intermediate signals
    std::vector<float> m_mid;
    std::vector<float> m_side;
    std::vector<float> m_scratch;

    static constexpr float kCenterGain     = 0.707f;
    static constexpr float kLfeGain        = 0.5f;
    static constexpr float kSurrGain       = 0.55f;
    static constexpr float kBackGain       = 0.4f;
    static constexpr float kDirectBleed    = 0.2f;
    static constexpr float kHeightGain     = 0.35f;
    static constexpr float kRearHeightGain = 0.25f;
};

} // namespace MagicSpatial
