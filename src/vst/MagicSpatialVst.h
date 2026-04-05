#pragma once

#include <windows.h>
#include <string>
#include "vst/VstDefs.h"
#include "processing/UpmixEngine.h"
#include "processing/ChannelLayout.h"
#include "processing/MultibandSplitter.h"
#include "processing/SpectralSeparator.h"
#include "processing/TransientDetector.h"
#include "processing/StereoCorrelationAnalyzer.h"
#include "processing/Decorrelator.h"
#include "processing/Biquad.h"
#include "audio/SpatialObjectWriter.h"
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
//   1 - Speakers:  2.0 / 5.1.2 / 5.1.4 / 7.1.2 / 7.1.4
class MagicSpatialVst {
public:
    static constexpr int kNumInputs  = 12;
    static constexpr int kNumOutputs = 12;
    static constexpr int kNumParams  = 2;

    static constexpr VstInt32 kUniqueID = 'MgSp';

    // EditorWndProc needs direct member access to avoid SetParameter
    // (which causes E-APO to reload the plugin)
    friend LRESULT CALLBACK EditorWndProc(HWND, UINT, WPARAM, LPARAM);

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
    ERect m_editorRect{0, 0, 190, 340};
    void* m_editorHwnd = nullptr;  // HWND of our child window

    // Parameters
    float m_paramMode        = 0.0f;  // Auto
    float m_paramSpeakers    = 0.25f; // 5.1.2 (default)

    // Processing state
    UpmixEngine m_engine;
    float m_sampleRate = 48000.0f;
    VstInt32 m_blockSize = 4096;
    bool m_engineInitialized = false;
    InputLayout m_currentLayout = InputLayout::Unknown;

    // In-process spatial object output via ISpatialAudioClient
    SpatialObjectWriter m_spatialWriter;
    bool m_spatialInitAttempted = false;

    // Spatial decomposition DSP (used when spatial objects are active)
    SpectralSeparator m_spatialSeparator;
    MultibandSplitter m_spatialSplitter;
    StereoCorrelationAnalyzer m_spatialCorrelation;
    TransientDetector m_spatialTransients;
    BiquadFilter m_spatialLfeLowpass;
    Decorrelator m_spatialDecorr[8];
    bool m_spatialDspInitialized = false;

    // Pre-allocated scratch buffers (avoids heap allocation on audio thread)
    std::vector<float> m_sBandL[4], m_sBandR[4];
    std::vector<float> m_sSide2, m_sSide3;
    std::vector<float> m_sFullMid, m_sTransients, m_sScratch;
    std::vector<float> m_sSurrL, m_sSurrR, m_sHeightL, m_sHeightR;
    std::vector<float> m_sAmbL, m_sAmbR;

    // Spectral separator outputs + residuals (delayed L/R with per-bin centre peeled)
    std::vector<float> m_sDelayedL, m_sDelayedR, m_sCenter;
    std::vector<float> m_sResidualL, m_sResidualR;

    // Smoothed surround weights (per-band, smoothed across blocks)
    float m_smoothSW[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    static constexpr float kSWSmoothing = 0.05f; // smoothing factor per block

    // Smoothed spectral brightness (0 = all bass, 1 = all treble). Drives the
    // elevation of OBJ_HEIGHT_LEFT/RIGHT via the Pratt effect: bright content
    // floats upward, dark content settles back toward the reference 45°.
    float m_smoothHeightBrightness = 0.25f;
    static constexpr float kBrightnessSmoothing = 0.08f; // ~150ms ramp at 48k/480-sample blocks

    // Smoothed vocal pitch estimate (0 = low/chest voice, 1 = soprano/head voice).
    // Drives OBJ_VOCAL elevation so a head-voice soars slightly upward while a
    // deep male narrator sits at ear level. Derived from the zero-crossing rate
    // of the spectral centre stream, energy-gated so silence holds the last
    // value instead of drifting.
    float m_smoothVocalPitch = 0.0f;
    static constexpr float kPitchSmoothing = 0.10f; // ~100ms

    // Smoothed L/R energy balance (-1 = fully left, +1 = fully right). Drives
    // the horizontal steering of OBJ_LEFT/RIGHT around the reference ±30° base.
    // Smoothed so abrupt pans do not produce per-block position zippering.
    // IMPORTANT: this scalar only affects POSITION, never gain or surround
    // weighting, so it cannot mute the rear channels.
    float m_smoothBalance = 0.0f;
    static constexpr float kBalanceSmoothing = 0.06f; // ~120ms ramp at block rate

    void InitSpatialDsp();
    void ProcessSpatialObjects(float** inputs, float** outputs, VstInt32 sampleFrames);
};

} // namespace MagicSpatial
