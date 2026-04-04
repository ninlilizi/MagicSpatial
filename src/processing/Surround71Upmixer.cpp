#include "processing/Surround71Upmixer.h"
#include <cstring>

namespace MagicSpatial {

void Surround71Upmixer::Initialize(uint32_t sampleRate, uint32_t maxFrameCount) {
    float sr = static_cast<float>(sampleRate);

    auto hpCoeffs = DesignHighpass(3000.0f, sr);
    for (auto& hp : m_heightHighpass) {
        hp.SetCoeffs(hpCoeffs);
    }

    const auto* presets = GetDecorrelatorPresets();
    m_heightDecorrelators[0].Initialize(presets[4].coefficients, 3, presets[4].delaySamples);
    m_heightDecorrelators[1].Initialize(presets[5].coefficients, 3, presets[5].delaySamples);
    m_heightDecorrelators[2].Initialize(presets[6].coefficients, 3, presets[6].delaySamples);
    m_heightDecorrelators[3].Initialize(presets[7].coefficients, 3, presets[7].delaySamples);

    m_scratch.resize(maxFrameCount);
    m_blend.resize(maxFrameCount);
}

void Surround71Upmixer::Process(const float* const* input, uint32_t frameCount, float** output) {
    // 7.1 channel order: [0]=FL, [1]=FR, [2]=C, [3]=LFE, [4]=BL, [5]=BR, [6]=SL, [7]=SR

    // Passthrough all 8 bed channels
    std::memcpy(output[CH_FL],  input[0], frameCount * sizeof(float));
    std::memcpy(output[CH_FR],  input[1], frameCount * sizeof(float));
    std::memcpy(output[CH_C],   input[2], frameCount * sizeof(float));
    std::memcpy(output[CH_LFE], input[3], frameCount * sizeof(float));
    std::memcpy(output[CH_BL],  input[4], frameCount * sizeof(float));
    std::memcpy(output[CH_BR],  input[5], frameCount * sizeof(float));
    std::memcpy(output[CH_SL],  input[6], frameCount * sizeof(float));
    std::memcpy(output[CH_SR],  input[7], frameCount * sizeof(float));

    // TFL: from FL
    m_heightHighpass[0].Process(input[0], m_scratch.data(), frameCount);
    m_heightDecorrelators[0].Process(m_scratch.data(), output[CH_TFL], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) output[CH_TFL][i] *= kHeightGain;

    // TFR: from FR
    m_heightHighpass[1].Process(input[1], m_scratch.data(), frameCount);
    m_heightDecorrelators[1].Process(m_scratch.data(), output[CH_TFR], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) output[CH_TFR][i] *= kHeightGain;

    // TBL: blend of BL + SL
    for (uint32_t i = 0; i < frameCount; ++i) {
        m_blend[i] = input[4][i] * 0.5f + input[6][i] * 0.5f;
    }
    m_heightHighpass[2].Process(m_blend.data(), m_scratch.data(), frameCount);
    m_heightDecorrelators[2].Process(m_scratch.data(), output[CH_TBL], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) output[CH_TBL][i] *= kHeightGain;

    // TBR: blend of BR + SR
    for (uint32_t i = 0; i < frameCount; ++i) {
        m_blend[i] = input[5][i] * 0.5f + input[7][i] * 0.5f;
    }
    m_heightHighpass[3].Process(m_blend.data(), m_scratch.data(), frameCount);
    m_heightDecorrelators[3].Process(m_scratch.data(), output[CH_TBR], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) output[CH_TBR][i] *= kHeightGain;
}

void Surround71Upmixer::Reset() {
    for (auto& hp : m_heightHighpass) hp.Reset();
    for (auto& d : m_heightDecorrelators) d.Reset();
}

} // namespace MagicSpatial
