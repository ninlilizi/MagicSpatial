#include "processing/Surround51Upmixer.h"
#include <cstring>

namespace MagicSpatial {

void Surround51Upmixer::Initialize(uint32_t sampleRate, uint32_t maxFrameCount) {
    float sr = static_cast<float>(sampleRate);

    auto hpCoeffs = DesignHighpass(3000.0f, sr);
    for (auto& hp : m_heightHighpass) {
        hp.SetCoeffs(hpCoeffs);
    }

    const auto* presets = GetDecorrelatorPresets();
    m_backDecorrelators[0].Initialize(presets[2].coefficients, 3, presets[2].delaySamples);
    m_backDecorrelators[1].Initialize(presets[3].coefficients, 3, presets[3].delaySamples);
    m_heightDecorrelators[0].Initialize(presets[4].coefficients, 3, presets[4].delaySamples);
    m_heightDecorrelators[1].Initialize(presets[5].coefficients, 3, presets[5].delaySamples);
    m_heightDecorrelators[2].Initialize(presets[6].coefficients, 3, presets[6].delaySamples);
    m_heightDecorrelators[3].Initialize(presets[7].coefficients, 3, presets[7].delaySamples);

    m_scratch.resize(maxFrameCount);
}

void Surround51Upmixer::Process(const float* const* input, uint32_t frameCount, float** output) {
    // 5.1 channel order: [0]=FL, [1]=FR, [2]=C, [3]=LFE, [4]=SL, [5]=SR

    // Passthrough the 6 input channels
    std::memcpy(output[CH_FL],  input[0], frameCount * sizeof(float));
    std::memcpy(output[CH_FR],  input[1], frameCount * sizeof(float));
    std::memcpy(output[CH_C],   input[2], frameCount * sizeof(float));
    std::memcpy(output[CH_LFE], input[3], frameCount * sizeof(float));
    std::memcpy(output[CH_SL],  input[4], frameCount * sizeof(float));
    std::memcpy(output[CH_SR],  input[5], frameCount * sizeof(float));

    // Synthesize BL/BR from SL/SR via decorrelation
    m_backDecorrelators[0].Process(input[4], m_scratch.data(), frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        output[CH_BL][i] = m_scratch[i] * kBackBlend + input[4][i] * kBackDirect;
    }
    m_backDecorrelators[1].Process(input[5], m_scratch.data(), frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        output[CH_BR][i] = m_scratch[i] * kBackBlend + input[5][i] * kBackDirect;
    }

    // Height channels
    m_heightHighpass[0].Process(output[CH_FL], m_scratch.data(), frameCount);
    m_heightDecorrelators[0].Process(m_scratch.data(), output[CH_TFL], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) output[CH_TFL][i] *= kHeightGain;

    m_heightHighpass[1].Process(output[CH_FR], m_scratch.data(), frameCount);
    m_heightDecorrelators[1].Process(m_scratch.data(), output[CH_TFR], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) output[CH_TFR][i] *= kHeightGain;

    m_heightHighpass[2].Process(output[CH_SL], m_scratch.data(), frameCount);
    m_heightDecorrelators[2].Process(m_scratch.data(), output[CH_TBL], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) output[CH_TBL][i] *= kRearHeightGain;

    m_heightHighpass[3].Process(output[CH_SR], m_scratch.data(), frameCount);
    m_heightDecorrelators[3].Process(m_scratch.data(), output[CH_TBR], frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) output[CH_TBR][i] *= kRearHeightGain;
}

void Surround51Upmixer::Reset() {
    for (auto& hp : m_heightHighpass) hp.Reset();
    for (auto& d : m_backDecorrelators) d.Reset();
    for (auto& d : m_heightDecorrelators) d.Reset();
}

} // namespace MagicSpatial
