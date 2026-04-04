#pragma once

#include <cstdint>

namespace MagicSpatial {

// Computes running stereo correlation per frequency band using single-pole smoothing.
// +1 = mono/center (L and R identical), 0 = uncorrelated, -1 = anti-phase (pure side)
class StereoCorrelationAnalyzer {
public:
    static constexpr int kNumBands = 4;

    void Initialize(float sampleRate);

    // Compute smoothed correlation for one band over this block.
    // Returns value in [-1, +1].
    float ProcessBand(int bandIndex, const float* bandL, const float* bandR, uint32_t frameCount);

    void Reset();

private:
    float m_smoothCoeff = 0.0f; // per-sample coefficient (~50ms time constant)

    struct BandState {
        float sumLR = 0.0f;
        float sumLL = 0.0f;
        float sumRR = 0.0f;
    };

    BandState m_bands[kNumBands];
};

} // namespace MagicSpatial
