#pragma once

#include "processing/ChannelLayout.h"
#include "processing/EnhancedStereoUpmixer.h"
#include "processing/Surround51Upmixer.h"
#include "processing/Surround71Upmixer.h"
#include "core/Types.h"

#include <cstdint>

namespace MagicSpatial {

// Dispatches to the appropriate upmixer based on input layout.
class UpmixEngine {
public:
    void Initialize(InputLayout layout, uint32_t sampleRate, uint32_t maxFrameCount);

    // Process non-interleaved input channels into 12 output channels.
    void Process(const float* const* inputChannels, int inputChannelCount,
                 uint32_t frameCount, float** outputChannels);

    InputLayout GetCurrentLayout() const { return m_layout; }
    void Reset();

private:
    InputLayout m_layout = InputLayout::Unknown;
    EnhancedStereoUpmixer m_stereo;
    Surround51Upmixer m_surround51;
    Surround71Upmixer m_surround71;
};

} // namespace MagicSpatial
