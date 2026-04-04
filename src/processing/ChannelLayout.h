#pragma once

#include <cstdint>

namespace MagicSpatial {

// Input format layouts
enum class InputLayout {
    Stereo,      // 2 channels: FL, FR
    Surround51,  // 6 channels: FL, FR, C, LFE, SL, SR (or BL, BR)
    Surround71,  // 8 channels: FL, FR, C, LFE, BL, BR, SL, SR
    Passthrough, // Already 7.1.4 — just pass through
    Unknown
};

// Map a channel count to a layout
inline InputLayout LayoutFromChannelCount(int channels) {
    switch (channels) {
    case 2:  return InputLayout::Stereo;
    case 6:  return InputLayout::Surround51;
    case 8:  return InputLayout::Surround71;
    case 12: return InputLayout::Passthrough;
    default: return InputLayout::Unknown;
    }
}

} // namespace MagicSpatial
