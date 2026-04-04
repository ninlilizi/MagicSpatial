#pragma once

#include <cstdint>
#include <array>

namespace MagicSpatial {

// 7.1.4 has 12 channels
inline constexpr int kAtmosChannelCount = 12;

// Channel indices within our 7.1.4 layout
enum AtmosChannel : int {
    CH_FL   = 0,   // Front Left
    CH_FR   = 1,   // Front Right
    CH_C    = 2,   // Center
    CH_LFE  = 3,   // Low Frequency Effects
    CH_SL   = 4,   // Side Left
    CH_SR   = 5,   // Side Right
    CH_BL   = 6,   // Back Left
    CH_BR   = 7,   // Back Right
    CH_TFL  = 8,   // Top Front Left
    CH_TFR  = 9,   // Top Front Right
    CH_TBL  = 10,  // Top Back Left
    CH_TBR  = 11,  // Top Back Right
};

// A frame of 7.1.4 output: 12 mono sample values
using AtmosFrame = std::array<float, kAtmosChannelCount>;

} // namespace MagicSpatial
