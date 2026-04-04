#include "processing/EnhancedStereoUpmixer.h"
#include <cstring>

namespace MagicSpatial {

void EnhancedStereoUpmixer::Initialize(uint32_t sampleRate, uint32_t maxFrameCount) {
    float sr = static_cast<float>(sampleRate);

    m_splitter.Initialize(sr, maxFrameCount);
    m_correlation.Initialize(sr);
    m_transientDetector.Initialize(sr);
    m_lfeLowpass.SetCoeffs(DesignLowpass(80.0f, sr));

    // Initialize decorrelators from presets, keyed by target channel
    const auto* presets = GetDecorrelatorPresets();

    // Band 1 (low-mid): [0]=SL(preset0), [1]=SR(preset1)
    m_band1Decorr[0].Initialize(presets[0].coefficients, 3, presets[0].delaySamples);
    m_band1Decorr[1].Initialize(presets[1].coefficients, 3, presets[1].delaySamples);

    // Band 2 (high-mid): SL, SR, BL, BR, TFL, TFR, TBL, TBR
    for (int i = 0; i < 8; ++i) {
        m_band2Decorr[i].Initialize(presets[i].coefficients, 3, presets[i].delaySamples);
    }

    // Band 3 (treble): [0]=SL, [1]=SR, [2]=TFL, [3]=TFR, [4]=TBL, [5]=TBR
    m_band3Decorr[0].Initialize(presets[0].coefficients, 3, presets[0].delaySamples);
    m_band3Decorr[1].Initialize(presets[1].coefficients, 3, presets[1].delaySamples);
    m_band3Decorr[2].Initialize(presets[4].coefficients, 3, presets[4].delaySamples);
    m_band3Decorr[3].Initialize(presets[5].coefficients, 3, presets[5].delaySamples);
    m_band3Decorr[4].Initialize(presets[6].coefficients, 3, presets[6].delaySamples);
    m_band3Decorr[5].Initialize(presets[7].coefficients, 3, presets[7].delaySamples);

    // Allocate scratch buffers
    for (int b = 0; b < MultibandSplitter::kNumBands; ++b) {
        m_bandL[b].resize(maxFrameCount);
        m_bandR[b].resize(maxFrameCount);
    }
    m_bandMid.resize(maxFrameCount);
    m_bandSide.resize(maxFrameCount);
    m_fullMid.resize(maxFrameCount);
    m_transients.resize(maxFrameCount);
    m_decorrScratch.resize(maxFrameCount);
}

void EnhancedStereoUpmixer::Process(const float* const* input, uint32_t frameCount, float** output) {
    const float* L = input[0];
    const float* R = input[1];

    // Front L/R: pass through the original source UNCHANGED.
    // All other channels are purely additive — we never subtract from the source.
    std::memcpy(output[CH_FL], L, frameCount * sizeof(float));
    std::memcpy(output[CH_FR], R, frameCount * sizeof(float));

    // Zero the synthesized channels (C, LFE, surrounds, heights)
    for (int ch = CH_C; ch < kAtmosChannelCount; ++ch) {
        std::memset(output[ch], 0, frameCount * sizeof(float));
    }

    // Step 1: Split stereo into 4 frequency bands
    float* bL[MultibandSplitter::kNumBands] = {
        m_bandL[0].data(), m_bandL[1].data(), m_bandL[2].data(), m_bandL[3].data()
    };
    float* bR[MultibandSplitter::kNumBands] = {
        m_bandR[0].data(), m_bandR[1].data(), m_bandR[2].data(), m_bandR[3].data()
    };
    m_splitter.Process(L, R, frameCount, bL, bR);

    // Step 2: Full-band transient detection
    for (uint32_t i = 0; i < frameCount; ++i) {
        m_fullMid[i] = (L[i] + R[i]) * 0.5f;
    }
    m_transientDetector.Process(m_fullMid.data(), m_transients.data(), frameCount);

    // Step 3: Per-band correlation
    float corr[MultibandSplitter::kNumBands];
    for (int b = 0; b < MultibandSplitter::kNumBands; ++b) {
        corr[b] = m_correlation.ProcessBand(b, bL[b], bR[b], frameCount);
    }

    // Derive routing weights from correlation
    float surroundWeight[MultibandSplitter::kNumBands];
    for (int b = 0; b < MultibandSplitter::kNumBands; ++b) {
        float cn = (corr[b] + 1.0f) * 0.5f;
        surroundWeight[b] = 1.0f - cn;
    }

    // ========== Band 0: Sub-bass (< 200Hz) ==========
    // Synthesize Center and LFE. Front L/R already have the full source.
    {
        for (uint32_t i = 0; i < frameCount; ++i) {
            m_bandMid[i] = (bL[0][i] + bR[0][i]) * 0.5f;
        }

        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_C][i] += m_bandMid[i] * kSubCenterGain;
        }

        m_lfeLowpass.Process(m_bandMid.data(), m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_LFE][i] += m_decorrScratch[i] * kSubLfeGain;
        }
    }

    // ========== Band 1: Low-mid (200Hz - 2kHz) ==========
    // Synthesize Center and Surrounds. Front L/R untouched.
    {
        float cw = (corr[1] + 1.0f) * 0.5f;
        float sw = surroundWeight[1];

        for (uint32_t i = 0; i < frameCount; ++i) {
            m_bandMid[i]  = (bL[1][i] + bR[1][i]) * 0.5f;
            m_bandSide[i] = (bL[1][i] - bR[1][i]) * 0.5f;
        }

        for (uint32_t i = 0; i < frameCount; ++i) {
            float tBoost = 1.0f + m_transients[i] * 0.3f;
            output[CH_C][i] += m_bandMid[i] * kLowMidCenterGain * cw * tBoost;
        }

        m_band1Decorr[0].Process(m_bandSide.data(), m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_SL][i] += m_decorrScratch[i] * kLowMidSurrGain * sw;
        }
        m_band1Decorr[1].Process(m_bandSide.data(), m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_SR][i] += m_decorrScratch[i] * kLowMidSurrGain * sw;
        }
    }

    // ========== Band 2: High-mid (2kHz - 8kHz) ==========
    // Synthesize Surrounds, Backs, Heights. Front L/R untouched.
    {
        float sw = surroundWeight[2];

        for (uint32_t i = 0; i < frameCount; ++i) {
            m_bandSide[i] = (bL[2][i] - bR[2][i]) * 0.5f;
        }

        m_band2Decorr[0].Process(m_bandSide.data(), m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            float tDuck = 1.0f - m_transients[i] * 0.5f;
            output[CH_SL][i] += m_decorrScratch[i] * kHighMidSurrGain * sw * tDuck;
        }
        m_band2Decorr[1].Process(m_bandSide.data(), m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            float tDuck = 1.0f - m_transients[i] * 0.5f;
            output[CH_SR][i] += m_decorrScratch[i] * kHighMidSurrGain * sw * tDuck;
        }

        m_band2Decorr[2].Process(m_bandSide.data(), m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_BL][i] += m_decorrScratch[i] * kHighMidBackGain * sw;
        }
        m_band2Decorr[3].Process(m_bandSide.data(), m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_BR][i] += m_decorrScratch[i] * kHighMidBackGain * sw;
        }

        m_band2Decorr[4].Process(bL[2], m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_TFL][i] += m_decorrScratch[i] * kHighMidHeightGain * sw;
        }
        m_band2Decorr[5].Process(bR[2], m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_TFR][i] += m_decorrScratch[i] * kHighMidHeightGain * sw;
        }
        m_band2Decorr[6].Process(bL[2], m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_TBL][i] += m_decorrScratch[i] * kHighMidBackHeightGain * sw;
        }
        m_band2Decorr[7].Process(bR[2], m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_TBR][i] += m_decorrScratch[i] * kHighMidBackHeightGain * sw;
        }
    }

    // ========== Band 3: Treble (> 8kHz) ==========
    // Synthesize Sides and Heights. Front L/R untouched.
    {
        float sw = surroundWeight[3];

        for (uint32_t i = 0; i < frameCount; ++i) {
            m_bandSide[i] = (bL[3][i] - bR[3][i]) * 0.5f;
        }

        m_band3Decorr[0].Process(m_bandSide.data(), m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_SL][i] += m_decorrScratch[i] * kTrebleSideGain * sw;
        }
        m_band3Decorr[1].Process(m_bandSide.data(), m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_SR][i] += m_decorrScratch[i] * kTrebleSideGain * sw;
        }

        m_band3Decorr[2].Process(bL[3], m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_TFL][i] += m_decorrScratch[i] * kTrebleHeightGain;
        }
        m_band3Decorr[3].Process(bR[3], m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_TFR][i] += m_decorrScratch[i] * kTrebleHeightGain;
        }
        m_band3Decorr[4].Process(bL[3], m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_TBL][i] += m_decorrScratch[i] * kTrebleBackHeightGain;
        }
        m_band3Decorr[5].Process(bR[3], m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_TBR][i] += m_decorrScratch[i] * kTrebleBackHeightGain;
        }
    }

    // Centre channel exception: subtract centre content from FL/FR so that
    // vocals/dialogue anchor to the physical centre speaker rather than
    // doubling as both phantom centre (in L/R) and discrete centre.
    for (uint32_t i = 0; i < frameCount; ++i) {
        output[CH_FL][i] -= output[CH_C][i] * 0.5f;
        output[CH_FR][i] -= output[CH_C][i] * 0.5f;
    }
}

void EnhancedStereoUpmixer::Reset() {
    m_splitter.Reset();
    m_correlation.Reset();
    m_transientDetector.Reset();
    m_lfeLowpass.Reset();
    for (auto& d : m_band1Decorr) d.Reset();
    for (auto& d : m_band2Decorr) d.Reset();
    for (auto& d : m_band3Decorr) d.Reset();
}

} // namespace MagicSpatial
