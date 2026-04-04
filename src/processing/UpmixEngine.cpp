#include "processing/UpmixEngine.h"
#include <cstring>

namespace MagicSpatial {

void UpmixEngine::Initialize(InputLayout layout, uint32_t sampleRate, uint32_t maxFrameCount) {
    m_layout = layout;

    switch (layout) {
    case InputLayout::Stereo:
        m_stereo.Initialize(sampleRate, maxFrameCount);
        break;
    case InputLayout::Surround51:
        m_surround51.Initialize(sampleRate, maxFrameCount);
        break;
    case InputLayout::Surround71:
        m_surround71.Initialize(sampleRate, maxFrameCount);
        break;
    default:
        break;
    }
}

void UpmixEngine::Process(const float* const* inputChannels, int inputChannelCount,
                           uint32_t frameCount, float** outputChannels) {
    switch (m_layout) {
    case InputLayout::Stereo:
        m_stereo.Process(inputChannels, frameCount, outputChannels);
        break;

    case InputLayout::Surround51:
        m_surround51.Process(inputChannels, frameCount, outputChannels);
        break;

    case InputLayout::Surround71:
        m_surround71.Process(inputChannels, frameCount, outputChannels);
        break;

    case InputLayout::Passthrough:
        // Copy first 12 input channels directly to output
        for (int ch = 0; ch < kAtmosChannelCount && ch < inputChannelCount; ++ch) {
            std::memcpy(outputChannels[ch], inputChannels[ch], frameCount * sizeof(float));
        }
        // Zero any remaining output channels
        for (int ch = inputChannelCount; ch < kAtmosChannelCount; ++ch) {
            std::memset(outputChannels[ch], 0, frameCount * sizeof(float));
        }
        break;

    default:
        // Output silence
        for (int ch = 0; ch < kAtmosChannelCount; ++ch) {
            std::memset(outputChannels[ch], 0, frameCount * sizeof(float));
        }
        break;
    }
}

void UpmixEngine::Reset() {
    m_stereo.Reset();
    m_surround51.Reset();
    m_surround71.Reset();
}

} // namespace MagicSpatial
