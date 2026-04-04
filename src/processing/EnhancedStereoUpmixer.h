#pragma once

#include "processing/Biquad.h"
#include "processing/Decorrelator.h"
#include "processing/MultibandSplitter.h"
#include "processing/TransientDetector.h"
#include "processing/StereoCorrelationAnalyzer.h"
#include "core/Types.h"

#include <cstdint>
#include <vector>

namespace MagicSpatial {

// Enhanced stereo-to-7.1.4 upmixer with frequency-dependent spatial steering,
// correlation-based center/surround routing, and transient detection.
// Zero added latency (all biquad IIR + envelope followers).
class EnhancedStereoUpmixer {
public:
    void Initialize(uint32_t sampleRate, uint32_t maxFrameCount);
    void Process(const float* const* input, uint32_t frameCount, float** output);
    void Reset();

private:
    // Sub-components
    MultibandSplitter m_splitter;
    StereoCorrelationAnalyzer m_correlation;
    TransientDetector m_transientDetector;
    BiquadFilter m_lfeLowpass; // 80Hz, further shapes sub-bass for LFE

    // Decorrelators: separate instances per band to avoid state conflicts
    // Band 1 (low-mid): SL, SR
    Decorrelator m_band1Decorr[2];
    // Band 2 (high-mid): SL, SR, BL, BR, TFL, TFR, TBL, TBR
    Decorrelator m_band2Decorr[8];
    // Band 3 (treble): SL, SR, TFL, TFR, TBL, TBR
    Decorrelator m_band3Decorr[6];

    // Scratch buffers
    std::vector<float> m_bandL[MultibandSplitter::kNumBands];
    std::vector<float> m_bandR[MultibandSplitter::kNumBands];
    std::vector<float> m_bandMid;
    std::vector<float> m_bandSide;
    std::vector<float> m_fullMid;
    std::vector<float> m_transients;
    std::vector<float> m_decorrScratch;

    // Gain constants
    // Sub-bass (< 200Hz)
    static constexpr float kSubCenterGain    = 0.707f;
    static constexpr float kSubLfeGain       = 0.50f;
    // Low-mid (200Hz - 2kHz)
    static constexpr float kLowMidFrontGain  = 0.80f;
    static constexpr float kLowMidCenterGain = 0.60f;
    static constexpr float kLowMidSurrGain   = 0.35f;
    // High-mid (2kHz - 8kHz)
    static constexpr float kHighMidFrontGain = 0.65f;
    static constexpr float kHighMidSurrGain  = 0.50f;
    static constexpr float kHighMidBackGain  = 0.30f;
    static constexpr float kHighMidHeightGain     = 0.20f;
    static constexpr float kHighMidBackHeightGain = 0.15f;
    // Treble (> 8kHz)
    static constexpr float kTrebleFrontGain       = 0.40f;
    static constexpr float kTrebleSideGain        = 0.50f;
    static constexpr float kTrebleHeightGain      = 0.45f;
    static constexpr float kTrebleBackHeightGain  = 0.30f;
};

} // namespace MagicSpatial
