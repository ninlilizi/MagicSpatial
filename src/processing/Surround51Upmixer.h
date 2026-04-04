#pragma once

#include "processing/Biquad.h"
#include "processing/Decorrelator.h"
#include "core/Types.h"

#include <cstdint>
#include <vector>

namespace MagicSpatial {

// Upmixes 5.1 to 7.1.4 Atmos bed.
// Input channel order: FL, FR, C, LFE, SL, SR
// Synthesizes BL/BR from existing surrounds and height channels from front + surround.
class Surround51Upmixer {
public:
    void Initialize(uint32_t sampleRate, uint32_t maxFrameCount);

    // Input: 6 separate channel pointers. Output: 12 mono channel pointers.
    void Process(const float* const* input, uint32_t frameCount, float** output);

    void Reset();

private:
    BiquadFilter m_heightHighpass[4];
    Decorrelator m_backDecorrelators[2];
    Decorrelator m_heightDecorrelators[4];
    std::vector<float> m_scratch;

    static constexpr float kBackBlend      = 0.7f;
    static constexpr float kBackDirect     = 0.3f;
    static constexpr float kHeightGain     = 0.35f;
    static constexpr float kRearHeightGain = 0.25f;
};

} // namespace MagicSpatial
