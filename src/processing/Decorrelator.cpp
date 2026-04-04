#include "processing/Decorrelator.h"
#include <cstring>

namespace MagicSpatial {

static const DecorrelatorConfig s_presets[8] = {
    {{ 0.3f, -0.5f,  0.7f},  7},   // A: SL
    {{-0.4f,  0.6f, -0.3f}, 11},   // B: SR
    {{ 0.5f, -0.7f,  0.4f}, 13},   // C: BL
    {{-0.6f,  0.3f, -0.5f}, 17},   // D: BR
    {{ 0.2f, -0.8f,  0.6f}, 23},   // E: TFL
    {{-0.3f,  0.7f, -0.4f}, 29},   // F: TFR
    {{ 0.4f, -0.6f,  0.5f}, 31},   // G: TBL
    {{-0.5f,  0.4f, -0.7f}, 37},   // H: TBR
};

const DecorrelatorConfig* GetDecorrelatorPresets() {
    return s_presets;
}

Decorrelator::Decorrelator(std::initializer_list<float> coefficients, size_t delaySamples) {
    Initialize(coefficients.begin(), coefficients.size(), delaySamples);
}

void Decorrelator::Initialize(const float* coefficients, size_t numCoeffs, size_t delaySamples) {
    m_sections.resize(numCoeffs);
    for (size_t i = 0; i < numCoeffs; ++i) {
        m_sections[i].coeff = coefficients[i];
        m_sections[i].state = 0;
    }

    m_delayLength = delaySamples;
    if (m_delayLength > 0) {
        m_delayLine.resize(m_delayLength, 0.0f);
    }
    else {
        m_delayLine.clear();
    }
    m_writeIndex = 0;
}

void Decorrelator::Process(const float* input, float* output, uint32_t frameCount) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        float sample = input[i];

        // Pre-delay
        if (m_delayLength > 0) {
            size_t readIndex = m_writeIndex;  // reads the oldest sample
            float delayed = m_delayLine[readIndex];
            m_delayLine[m_writeIndex] = sample;
            m_writeIndex = (m_writeIndex + 1) % m_delayLength;
            sample = delayed;
        }

        // Allpass cascade
        for (auto& section : m_sections) {
            // First-order allpass: H(z) = (a + z^-1) / (1 + a*z^-1)
            float w = sample - section.coeff * section.state;
            float y = section.coeff * w + section.state;
            section.state = w;
            sample = y;
        }

        output[i] = sample;
    }
}

void Decorrelator::Reset() {
    for (auto& s : m_sections) {
        s.state = 0;
    }
    std::memset(m_delayLine.data(), 0, m_delayLine.size() * sizeof(float));
    m_writeIndex = 0;
}

} // namespace MagicSpatial
