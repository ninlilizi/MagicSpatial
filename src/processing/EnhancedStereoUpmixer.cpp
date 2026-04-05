#include "processing/EnhancedStereoUpmixer.h"
#include <cstring>

namespace MagicSpatial {

void EnhancedStereoUpmixer::Initialize(uint32_t sampleRate, uint32_t maxFrameCount) {
    float sr = static_cast<float>(sampleRate);

    m_spectralSeparator.Initialize(sr, maxFrameCount);
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
    m_bandSide.resize(maxFrameCount);
    m_fullMid.resize(maxFrameCount);
    m_transients.resize(maxFrameCount);
    m_decorrScratch.resize(maxFrameCount);
    m_delayedL.resize(maxFrameCount);
    m_delayedR.resize(maxFrameCount);
    m_centerMono.resize(maxFrameCount);
    m_residualL.resize(maxFrameCount);
    m_residualR.resize(maxFrameCount);
}

void EnhancedStereoUpmixer::Process(const float* const* input, uint32_t frameCount, float** output) {
    const float* inL = input[0];
    const float* inR = input[1];

    // Step 0: Frequency-domain separation.
    // Produces delayed L/R (kFftSize-sample delay matching the OLA) and a
    // per-bin extracted mono centre. Residual L/R = delayed L/R - centre gives
    // clean vocal removal without touching overlapping neighbouring frequencies.
    m_spectralSeparator.Process(inL, inR, frameCount,
                                m_delayedL.data(), m_delayedR.data(), m_centerMono.data());

    const float* dL = m_delayedL.data();
    const float* dR = m_delayedR.data();
    const float* C  = m_centerMono.data();

    // Compute residuals (delayed L/R minus per-bin spectral centre). Everything
    // downstream except LFE runs on residuals so vocals NEVER reach the
    // surrounds or heights — not via side, not via direct band taps.
    for (uint32_t i = 0; i < frameCount; ++i) {
        m_residualL[i] = dL[i] - C[i];
        m_residualR[i] = dR[i] - C[i];
    }
    const float* rL = m_residualL.data();
    const float* rR = m_residualR.data();

    // Front L/R = residuals. CH_C = spectrally extracted centre.
    std::memcpy(output[CH_FL], rL, frameCount * sizeof(float));
    std::memcpy(output[CH_FR], rR, frameCount * sizeof(float));

    // Zero the synthesized channels (C, LFE, surrounds, heights) then seed the
    // centre with the spectral extraction.
    for (int ch = CH_C; ch < kAtmosChannelCount; ++ch) {
        std::memset(output[ch], 0, frameCount * sizeof(float));
    }
    std::memcpy(output[CH_C], C, frameCount * sizeof(float));

    // Step 1: Split RESIDUAL stereo into 4 frequency bands. Because the centre
    // is already peeled off, every downstream feed (side-based surrounds AND
    // direct band-tap heights) is automatically centre-free.
    float* bL[MultibandSplitter::kNumBands] = {
        m_bandL[0].data(), m_bandL[1].data(), m_bandL[2].data(), m_bandL[3].data()
    };
    float* bR[MultibandSplitter::kNumBands] = {
        m_bandR[0].data(), m_bandR[1].data(), m_bandR[2].data(), m_bandR[3].data()
    };
    m_splitter.Process(rL, rR, frameCount, bL, bR);

    // Step 2: Full-band transient detection on the ORIGINAL delayed mid. We use
    // the original (not residual) here because percussive transients are often
    // centred (kicks, snares) and we still want to detect them.
    for (uint32_t i = 0; i < frameCount; ++i) {
        m_fullMid[i] = (dL[i] + dR[i]) * 0.5f;
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

    // ========== LFE (< 80Hz) ==========
    // Fed from the ORIGINAL delayed mid (m_fullMid), NOT from the residual
    // band 0, so that centred sub content (kicks, bass guitar, synths) still
    // reaches the sub instead of vanishing into the centre object.
    {
        m_lfeLowpass.Process(m_fullMid.data(), m_decorrScratch.data(), frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            output[CH_LFE][i] += m_decorrScratch[i] * kSubLfeGain;
        }
    }

    // ========== Band 1: Low-mid (200Hz - 2kHz) ==========
    // Surrounds only. Per-bin spectral centre.
    {
        float sw = surroundWeight[1];

        for (uint32_t i = 0; i < frameCount; ++i) {
            m_bandSide[i] = (bL[1][i] - bR[1][i]) * 0.5f;
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
}

void EnhancedStereoUpmixer::Reset() {
    m_spectralSeparator.Reset();
    m_splitter.Reset();
    m_correlation.Reset();
    m_transientDetector.Reset();
    m_lfeLowpass.Reset();
    for (auto& d : m_band1Decorr) d.Reset();
    for (auto& d : m_band2Decorr) d.Reset();
    for (auto& d : m_band3Decorr) d.Reset();
}

} // namespace MagicSpatial
