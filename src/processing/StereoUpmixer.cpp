#include "processing/StereoUpmixer.h"

namespace MagicSpatial {

void StereoUpmixer::Initialize(uint32_t sampleRate, uint32_t maxFrameCount) {
    float sr = static_cast<float>(sampleRate);

    m_centerLowpass.SetCoeffs(DesignLowpass(500.0f, sr));
    m_lfeLowpass.SetCoeffs(DesignLowpass(80.0f, sr));

    auto hpCoeffs = DesignHighpass(3000.0f, sr);
    for (auto& hp : m_heightHighpass) {
        hp.SetCoeffs(hpCoeffs);
    }

    const auto* presets = GetDecorrelatorPresets();
    for (int i = 0; i < 8; ++i) {
        m_decorrelators[i].Initialize(
            presets[i].coefficients, 3, presets[i].delaySamples);
    }

    m_mid.resize(maxFrameCount);
    m_side.resize(maxFrameCount);
    m_scratch.resize(maxFrameCount);
}

void StereoUpmixer::Process(const float* const* input, uint32_t frameCount, float** output) {
    const float* L = input[0];
    const float* R = input[1];

    // Step 1: Compute Mid/Side
    for (uint32_t i = 0; i < frameCount; ++i) {
        m_mid[i]  = (L[i] + R[i]) * 0.5f;
        m_side[i] = (L[i] - R[i]) * 0.5f;
    }

    // Step 2: Center — lowpass-filtered Mid
    m_centerLowpass.Process(m_mid.data(), output[CH_C], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        output[CH_C][i] *= kCenterGain;
    }

    // Step 3: LFE — lowpass-filtered Mid
    m_lfeLowpass.Process(m_mid.data(), output[CH_LFE], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        output[CH_LFE][i] *= kLfeGain;
    }

    // Step 4: Front L/R — original with center bleed subtracted
    for (uint32_t i = 0; i < frameCount; ++i) {
        output[CH_FL][i] = L[i] - output[CH_C][i] * 0.5f;
        output[CH_FR][i] = R[i] - output[CH_C][i] * 0.5f;
    }

    // Step 5: Surround channels — decorrelated Side signal
    m_decorrelators[0].Process(m_side.data(), output[CH_SL], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        output[CH_SL][i] = output[CH_SL][i] * kSurrGain + L[i] * kDirectBleed;
    }

    m_decorrelators[1].Process(m_side.data(), output[CH_SR], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        output[CH_SR][i] = output[CH_SR][i] * kSurrGain + R[i] * kDirectBleed;
    }

    m_decorrelators[2].Process(m_side.data(), output[CH_BL], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        output[CH_BL][i] *= kBackGain;
    }

    m_decorrelators[3].Process(m_side.data(), output[CH_BR], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        output[CH_BR][i] *= kBackGain;
    }

    // Step 6: Height channels — highpass filtered front/surround -> decorrelated
    m_heightHighpass[0].Process(output[CH_FL], m_scratch.data(), frameCount);
    m_decorrelators[4].Process(m_scratch.data(), output[CH_TFL], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) output[CH_TFL][i] *= kHeightGain;

    m_heightHighpass[1].Process(output[CH_FR], m_scratch.data(), frameCount);
    m_decorrelators[5].Process(m_scratch.data(), output[CH_TFR], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) output[CH_TFR][i] *= kHeightGain;

    m_heightHighpass[2].Process(output[CH_SL], m_scratch.data(), frameCount);
    m_decorrelators[6].Process(m_scratch.data(), output[CH_TBL], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) output[CH_TBL][i] *= kRearHeightGain;

    m_heightHighpass[3].Process(output[CH_SR], m_scratch.data(), frameCount);
    m_decorrelators[7].Process(m_scratch.data(), output[CH_TBR], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) output[CH_TBR][i] *= kRearHeightGain;
}

void StereoUpmixer::Reset() {
    m_centerLowpass.Reset();
    m_lfeLowpass.Reset();
    for (auto& hp : m_heightHighpass) hp.Reset();
    for (auto& d : m_decorrelators) d.Reset();
}

} // namespace MagicSpatial
