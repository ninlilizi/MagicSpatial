#pragma once

#include "vst/VstDefs.h"
#include "processing/UpmixEngine.h"
#include "processing/ChannelLayout.h"
#include "core/Types.h"

namespace MagicSpatial {

// Physical speaker layouts supported by the plugin.
// Determines which output channels are active (rest are zeroed).
enum class SpeakerLayout {
    Layout_20,   // 2.0: Headphones — FL,FR only (all others folded down)
    Layout_512,  // 5.1.2: FL,FR,C,LFE,SL,SR + TFL,TFR
    Layout_514,  // 5.1.4: FL,FR,C,LFE,SL,SR + TFL,TFR,TBL,TBR
    Layout_712,  // 7.1.2: FL,FR,C,LFE,SL,SR,BL,BR + TFL,TFR
    Layout_714,  // 7.1.4: all 12 channels
};

// VST2 plugin: upmixes stereo/5.1/7.1 to Atmos bed for a given speaker layout.
// 12 inputs, 12 outputs. Unused input channels should be silent.
//
// Parameters:
//   0 - Mode:     Auto / Stereo / 5.1 / 7.1 / Passthrough
//   1 - Speakers:  5.1.2 / 5.1.4 / 7.1.2 / 7.1.4
//   2 - Height Gain:  0-100% (0% = let Dolby Access synthesize)
//   3 - Surround Gain: 0-100% (scales synthesized surround channels)
class MagicSpatialVst {
public:
    static constexpr int kNumInputs  = 12;
    static constexpr int kNumOutputs = 12;
    static constexpr int kNumParams  = 4;

    static constexpr VstInt32 kUniqueID = 'MgSp';

    MagicSpatialVst(audioMasterCallback hostCallback);

    VstIntPtr Dispatcher(VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt);
    void ProcessReplacing(float** inputs, float** outputs, VstInt32 sampleFrames);
    void SetParameter(VstInt32 index, float value);
    float GetParameter(VstInt32 index);

    AEffect* GetAEffect() { return &m_effect; }

private:
    void UpdateEngine();
    InputLayout DetectLayoutFromInputs(float** inputs, VstInt32 sampleFrames);
    SpeakerLayout SpeakerLayoutFromParam() const;
    void ZeroUnusedChannels(float** outputs, VstInt32 sampleFrames, SpeakerLayout layout);
    void EditorRedraw();

    AEffect m_effect{};
    audioMasterCallback m_hostCallback;

    // Editor
    ERect m_editorRect{0, 0, 220, 340};
    void* m_editorHwnd = nullptr;  // HWND of our child window

    // Parameters
    float m_paramMode        = 0.0f;  // Auto
    float m_paramSpeakers    = 0.25f; // 5.1.2 (default)
    float m_paramHeightGain  = 0.0f;  // 0% = let Dolby Access synthesize heights
    float m_paramSurrGain    = 0.7f;  // 70%

    // Processing state
    UpmixEngine m_engine;
    float m_sampleRate = 48000.0f;
    VstInt32 m_blockSize = 4096;
    bool m_engineInitialized = false;
    InputLayout m_currentLayout = InputLayout::Unknown;
};

} // namespace MagicSpatial
