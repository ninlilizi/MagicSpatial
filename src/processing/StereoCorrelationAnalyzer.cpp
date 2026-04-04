#include "processing/StereoCorrelationAnalyzer.h"
#include <cmath>
#include <algorithm>

namespace MagicSpatial {

void StereoCorrelationAnalyzer::Initialize(float sampleRate) {
    // 50ms time constant for smoothing
    m_smoothCoeff = 1.0f - std::exp(-1.0f / (0.050f * sampleRate));
    Reset();
}

float StereoCorrelationAnalyzer::ProcessBand(int bandIndex,
    const float* bandL, const float* bandR, uint32_t frameCount)
{
    if (bandIndex < 0 || bandIndex >= kNumBands || frameCount == 0) return 0.0f;

    BandState& b = m_bands[bandIndex];

    // Compute per-block average statistics
    float blockLR = 0.0f, blockLL = 0.0f, blockRR = 0.0f;
    for (uint32_t i = 0; i < frameCount; ++i) {
        blockLR += bandL[i] * bandR[i];
        blockLL += bandL[i] * bandL[i];
        blockRR += bandR[i] * bandR[i];
    }

    float invN = 1.0f / static_cast<float>(frameCount);
    blockLR *= invN;
    blockLL *= invN;
    blockRR *= invN;

    // Block-rate smoothing: effective coefficient = 1 - (1-sampleCoeff)^frameCount
    float blockCoeff = 1.0f - std::pow(1.0f - m_smoothCoeff, static_cast<float>(frameCount));
    b.sumLR += blockCoeff * (blockLR - b.sumLR);
    b.sumLL += blockCoeff * (blockLL - b.sumLL);
    b.sumRR += blockCoeff * (blockRR - b.sumRR);

    // Pearson correlation
    float denom = std::sqrt(b.sumLL * b.sumRR) + 1e-10f;
    return std::clamp(b.sumLR / denom, -1.0f, 1.0f);
}

void StereoCorrelationAnalyzer::Reset() {
    for (auto& b : m_bands) {
        b.sumLR = 0.0f;
        b.sumLL = 0.0f;
        b.sumRR = 0.0f;
    }
}

} // namespace MagicSpatial
