#pragma once

#include "processing/Biquad.h"
#include "processing/Decorrelator.h"
#include "core/Types.h"

#include <cstdint>
#include <vector>

namespace MagicSpatial {

// Upmixes 7.1 to 7.1.4 Atmos bed.
// All 8 bed channels pass through. Only height channels are synthesized.
class Surround71Upmixer {
public:
    void Initialize(uint32_t sampleRate, uint32_t maxFrameCount);

    // Input: 8 separate channel pointers. Output: 12 mono channel pointers.
    void Process(const float* const* input, uint32_t frameCount, float** output);

    void Reset();

private:
    BiquadFilter m_heightHighpass[4];
    Decorrelator m_heightDecorrelators[4];
    std::vector<float> m_scratch;
    std::vector<float> m_blend;

    static constexpr float kHeightGain = 0.35f;
};

} // namespace MagicSpatial
