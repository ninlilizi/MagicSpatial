#include "vst/MagicSpatialVst.h"
#include "core/Log.h"

#include <windows.h>
#include <commctrl.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace MagicSpatial {


// --- Editor control IDs ---
enum EditorCtrlID {
    ID_MODE_COMBO     = 1001,
    ID_SPEAKERS_COMBO = 1002,
    ID_HEIGHT_SLIDER  = 1003,
    ID_SURROUND_SLIDER= 1004,
    ID_HEIGHT_LABEL   = 1005,
    ID_SURROUND_LABEL = 1006,
};

// --- Editor WndProc ---
static LRESULT CALLBACK EditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* plugin = reinterpret_cast<MagicSpatialVst*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_COMMAND:
        if (!plugin) break;
        if (HIWORD(wParam) == CBN_SELCHANGE) {
            int id = LOWORD(wParam);
            int sel = static_cast<int>(SendMessageW(reinterpret_cast<HWND>(lParam), CB_GETCURSEL, 0, 0));
            // All combos set members directly — calling SetParameter triggers
            // E-APO to reload the plugin, killing the spatial audio stream.
            if (id == ID_MODE_COMBO) {
                plugin->m_paramMode = static_cast<float>(sel) * 0.25f;
                plugin->m_engineInitialized = false;
            }
            else if (id == ID_SPEAKERS_COMBO) {
                plugin->m_paramSpeakers = (sel == 0) ? 0.0f : (sel == 1) ? 0.25f : (sel == 2) ? 0.45f : (sel == 3) ? 0.65f : 1.0f;
            }
        }
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdcStatic, RGB(220, 220, 220));
        SetBkColor(hdcStatic, RGB(40, 40, 40));
        static HBRUSH bgBrush = CreateSolidBrush(RGB(40, 40, 40));
        return reinterpret_cast<LRESULT>(bgBrush);
    }

    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(40, 40, 40));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        return 1;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// --- Static VST2 callbacks ---

static VstIntPtr VSTCALLBACK dispatcherCallback(AEffect* effect, VstInt32 opcode,
    VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
    auto* plugin = static_cast<MagicSpatialVst*>(effect->object);
    return plugin->Dispatcher(opcode, index, value, ptr, opt);
}

static void VSTCALLBACK processReplacingCallback(AEffect* effect,
    float** inputs, float** outputs, VstInt32 sampleFrames)
{
    auto* plugin = static_cast<MagicSpatialVst*>(effect->object);
    plugin->ProcessReplacing(inputs, outputs, sampleFrames);
}

static void VSTCALLBACK setParameterCallback(AEffect* effect, VstInt32 index, float value) {
    auto* plugin = static_cast<MagicSpatialVst*>(effect->object);
    plugin->SetParameter(index, value);
}

static float VSTCALLBACK getParameterCallback(AEffect* effect, VstInt32 index) {
    auto* plugin = static_cast<MagicSpatialVst*>(effect->object);
    return plugin->GetParameter(index);
}

// --- Constructor ---

MagicSpatialVst::MagicSpatialVst(audioMasterCallback hostCallback)
    : m_hostCallback(hostCallback)
{
    std::memset(&m_effect, 0, sizeof(m_effect));

    m_effect.magic            = kEffectMagic;
    m_effect.dispatcher       = dispatcherCallback;
    m_effect.processReplacing = processReplacingCallback;
    m_effect.setParameter     = setParameterCallback;
    m_effect.getParameter     = getParameterCallback;
    m_effect.numPrograms      = 1;
    m_effect.numParams        = kNumParams;
    m_effect.numInputs        = kNumInputs;
    m_effect.numOutputs       = kNumOutputs;
    m_effect.flags            = effFlagsCanReplacing | effFlagsHasEditor;
    m_effect.uniqueID         = kUniqueID;
    m_effect.version          = 3;
    m_effect.object           = this;

    m_effect.DEPRECATED_process = nullptr;
    m_effect.processDoubleReplacing = nullptr;
}

// --- Dispatcher ---

VstIntPtr MagicSpatialVst::Dispatcher(VstInt32 opcode, VstInt32 index,
    VstIntPtr value, void* ptr, float opt)
{
    switch (opcode) {
    case effOpen:
        // Attempt spatial audio activation
        if (!m_spatialInitAttempted) {
            m_spatialInitAttempted = true;
            m_spatialWriter.Initialize();
            LogMsg("effOpen: SpatialObjectWriter Initialize called\n");
        }

        return 0;

    case effClose:
        m_spatialWriter.Shutdown();
        delete this;
        return 0;

    case effSetSampleRate:
        m_sampleRate = opt;
        m_engineInitialized = false;
        return 0;

    case effSetBlockSize:
        m_blockSize = static_cast<VstInt32>(value);
        m_engineInitialized = false;
        return 0;

    case effMainsChanged:
        if (value == 0) m_engine.Reset();
        return 0;

    case effEditGetRect:
        if (ptr) *reinterpret_cast<ERect**>(ptr) = &m_editorRect;
        return 1;

    case effEditOpen: {
        HWND parent = static_cast<HWND>(ptr);
        if (!parent) return 0;

        // Init common controls for trackbar
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
        InitCommonControlsEx(&icc);

        // Register window class (once)
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = EditorWndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"MagicSpatialEditor";
            RegisterClassW(&wc);
            classRegistered = true;
        }

        int w = m_editorRect.right - m_editorRect.left;
        int h = m_editorRect.bottom - m_editorRect.top;

        HWND hwnd = CreateWindowExW(0, L"MagicSpatialEditor", L"",
            WS_CHILD | WS_VISIBLE, 0, 0, w, h,
            parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        m_editorHwnd = hwnd;

        // Store plugin pointer for WndProc
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        HINSTANCE hInst = GetModuleHandleW(nullptr);
        int y = 10;
        int labelW = 80;
        int ctrlX = 90;
        int ctrlW = w - ctrlX - 10;

        // --- Title ---
        CreateWindowW(L"STATIC", L"MagicSpatial Upmixer",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, y, w - 20, 20, hwnd, nullptr, hInst, nullptr);
        y += 30;

        // --- Mode combo ---
        CreateWindowW(L"STATIC", L"Mode:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, y + 2, labelW, 20, hwnd, nullptr, hInst, nullptr);
        HWND modeCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            ctrlX, y, ctrlW, 200, hwnd, reinterpret_cast<HMENU>(ID_MODE_COMBO), hInst, nullptr);
        SendMessageW(modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto"));
        SendMessageW(modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Stereo"));
        SendMessageW(modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"5.1"));
        SendMessageW(modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"7.1"));
        SendMessageW(modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Passthrough"));
        // Select current
        int modeSel = (m_paramMode < 0.125f) ? 0 : (m_paramMode < 0.375f) ? 1 :
                      (m_paramMode < 0.625f) ? 2 : (m_paramMode < 0.875f) ? 3 : 4;
        SendMessageW(modeCombo, CB_SETCURSEL, modeSel, 0);
        y += 30;

        // --- Speakers combo ---
        CreateWindowW(L"STATIC", L"Speakers:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, y + 2, labelW, 20, hwnd, nullptr, hInst, nullptr);
        HWND spkCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            ctrlX, y, ctrlW, 200, hwnd, reinterpret_cast<HMENU>(ID_SPEAKERS_COMBO), hInst, nullptr);
        SendMessageW(spkCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2.0 (Headphones)"));
        SendMessageW(spkCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"5.1.2"));
        SendMessageW(spkCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"5.1.4"));
        SendMessageW(spkCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"7.1.2"));
        SendMessageW(spkCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"7.1.4"));
        int spkSel = (m_paramSpeakers < 0.15f) ? 0 : (m_paramSpeakers < 0.35f) ? 1 :
                     (m_paramSpeakers < 0.55f) ? 2 : (m_paramSpeakers < 0.75f) ? 3 : 4;
        SendMessageW(spkCombo, CB_SETCURSEL, spkSel, 0);

        return 1;
    }

    case effEditClose:
        if (m_editorHwnd) {
            DestroyWindow(static_cast<HWND>(m_editorHwnd));
            m_editorHwnd = nullptr;
        }
        return 0;

    case effEditIdle:
        return 0;

    case effGetEffectName:
        if (ptr) std::strncpy(static_cast<char*>(ptr), "MagicSpatial", kVstMaxEffectNameLen);
        return 1;

    case effGetVendorString:
        if (ptr) std::strncpy(static_cast<char*>(ptr), "MagicSpatial", kVstMaxVendorStrLen);
        return 1;

    case effGetProductString:
        if (ptr) std::strncpy(static_cast<char*>(ptr), "MagicSpatial Upmixer", kVstMaxProductStrLen);
        return 1;

    case effGetVendorVersion:
        return 3000;

    case effGetParamName:
        if (ptr) {
            char* buf = static_cast<char*>(ptr);
            switch (index) {
            case 0: std::strncpy(buf, "Mode",     kVstMaxParamStrLen); break;
            case 1: std::strncpy(buf, "Speakers", kVstMaxParamStrLen); break;
            }
        }
        return 0;

    case effGetParamLabel:
        if (ptr) static_cast<char*>(ptr)[0] = '\0';
        return 0;

    case effGetParamDisplay:
        if (ptr) {
            char* buf = static_cast<char*>(ptr);
            switch (index) {
            case 0:
                if      (m_paramMode < 0.125f) std::strncpy(buf, "Auto",   kVstMaxParamStrLen);
                else if (m_paramMode < 0.375f) std::strncpy(buf, "Stereo", kVstMaxParamStrLen);
                else if (m_paramMode < 0.625f) std::strncpy(buf, "5.1",    kVstMaxParamStrLen);
                else if (m_paramMode < 0.875f) std::strncpy(buf, "7.1",    kVstMaxParamStrLen);
                else                           std::strncpy(buf, "Pass",   kVstMaxParamStrLen);
                break;
            case 1:
                if      (m_paramSpeakers < 0.15f) std::strncpy(buf, "2.0",   kVstMaxParamStrLen);
                else if (m_paramSpeakers < 0.35f) std::strncpy(buf, "5.1.2", kVstMaxParamStrLen);
                else if (m_paramSpeakers < 0.55f) std::strncpy(buf, "5.1.4", kVstMaxParamStrLen);
                else if (m_paramSpeakers < 0.75f) std::strncpy(buf, "7.1.2", kVstMaxParamStrLen);
                else                              std::strncpy(buf, "7.1.4", kVstMaxParamStrLen);
                break;
            }
        }
        return 0;

    case effCanDo:
    case effGetTailSize:
        return 0;

    default:
        return 0;
    }
}

// --- Parameters ---

void MagicSpatialVst::SetParameter(VstInt32 index, float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    switch (index) {
    case 0:
        m_paramMode = value;
        m_engineInitialized = false;
        break;
    case 1:
        m_paramSpeakers = value;
        break;
    }
}

float MagicSpatialVst::GetParameter(VstInt32 index) {
    switch (index) {
    case 0: return m_paramMode;
    case 1: return m_paramSpeakers;
    default: return 0.0f;
    }
}

// --- Processing ---

SpeakerLayout MagicSpatialVst::SpeakerLayoutFromParam() const {
    if      (m_paramSpeakers < 0.15f) return SpeakerLayout::Layout_20;
    else if (m_paramSpeakers < 0.35f) return SpeakerLayout::Layout_512;
    else if (m_paramSpeakers < 0.55f) return SpeakerLayout::Layout_514;
    else if (m_paramSpeakers < 0.75f) return SpeakerLayout::Layout_712;
    else                              return SpeakerLayout::Layout_714;
}

void MagicSpatialVst::ZeroUnusedChannels(float** outputs, VstInt32 sampleFrames, SpeakerLayout layout) {
    auto zeroChannel = [&](int ch) {
        std::memset(outputs[ch], 0, static_cast<size_t>(sampleFrames) * sizeof(float));
    };

    switch (layout) {
    case SpeakerLayout::Layout_20:
        // Headphones: fold all channels down into FL/FR, then zero the rest.
        // Mix center, surrounds, backs, and heights into the front pair.
        for (VstInt32 i = 0; i < sampleFrames; ++i) {
            outputs[CH_FL][i] += outputs[CH_C][i]   * 0.707f
                              +  outputs[CH_SL][i]  * 0.5f
                              +  outputs[CH_BL][i]  * 0.35f
                              +  outputs[CH_TFL][i] * 0.35f
                              +  outputs[CH_TBL][i] * 0.25f
                              +  outputs[CH_LFE][i] * 0.5f;
            outputs[CH_FR][i] += outputs[CH_C][i]   * 0.707f
                              +  outputs[CH_SR][i]  * 0.5f
                              +  outputs[CH_BR][i]  * 0.35f
                              +  outputs[CH_TFR][i] * 0.35f
                              +  outputs[CH_TBR][i] * 0.25f
                              +  outputs[CH_LFE][i] * 0.5f;
        }
        // Zero everything except FL/FR
        for (int ch = CH_C; ch < kAtmosChannelCount; ++ch) {
            zeroChannel(ch);
        }
        break;
    case SpeakerLayout::Layout_512:
        zeroChannel(CH_BL);
        zeroChannel(CH_BR);
        zeroChannel(CH_TBL);
        zeroChannel(CH_TBR);
        break;
    case SpeakerLayout::Layout_514:
        zeroChannel(CH_BL);
        zeroChannel(CH_BR);
        break;
    case SpeakerLayout::Layout_712:
        zeroChannel(CH_TBL);
        zeroChannel(CH_TBR);
        break;
    case SpeakerLayout::Layout_714:
        break;
    }
}

void MagicSpatialVst::ProcessReplacing(float** inputs, float** outputs, VstInt32 sampleFrames) {
    if (sampleFrames <= 0) return;

    // Detect input layout first — needed to decide spatial vs channel path
    InputLayout targetLayout;
    if (m_paramMode < 0.125f) {
        targetLayout = DetectLayoutFromInputs(inputs, sampleFrames);
    }
    else if (m_paramMode < 0.375f) {
        targetLayout = InputLayout::Stereo;
    }
    else if (m_paramMode < 0.625f) {
        targetLayout = InputLayout::Surround51;
    }
    else if (m_paramMode < 0.875f) {
        targetLayout = InputLayout::Surround71;
    }
    else {
        targetLayout = InputLayout::Passthrough;
    }

    // --- Spatial object mode ---
    // The spatial path only handles stereo input — it indexes inputs[0]/[1]
    // and ignores the rest. Multi-channel content (5.1/7.1/Atmos) must go
    // through the engine/channel path below so its rear and height channels
    // are not silenced. We therefore enter the spatial path only when the
    // RESOLVED layout is Stereo: explicit Stereo mode forced it at line 400,
    // or Auto mode detected stereo input.
    if (targetLayout == InputLayout::Stereo &&
        (m_paramMode < 0.125f || m_paramMode < 0.375f)) { // Auto or Stereo
        ProcessSpatialObjects(inputs, outputs, sampleFrames);

        if (m_spatialWriter.IsActive()) {
            // Spatial objects are rendering — zero channel output to avoid double playback
            for (int ch = 0; ch < kAtmosChannelCount; ++ch) {
                std::memset(outputs[ch], 0, sampleFrames * sizeof(float));
            }
        }
        // If writer isn't active yet, channel output from ProcessSpatialObjects
        // provides audio through E-APO's normal path as fallback.
        return;
    }

    if (!m_engineInitialized || targetLayout != m_currentLayout) {
        m_currentLayout = targetLayout;
        m_engine.Initialize(
            m_currentLayout,
            static_cast<uint32_t>(m_sampleRate),
            static_cast<uint32_t>(m_blockSize));
        m_engineInitialized = true;
    }

    int inputChCount;
    switch (m_currentLayout) {
    case InputLayout::Stereo:      inputChCount = 2;  break;
    case InputLayout::Surround51:  inputChCount = 6;  break;
    case InputLayout::Surround71:  inputChCount = 8;  break;
    case InputLayout::Passthrough: inputChCount = 12; break;
    default:                       inputChCount = 2;  break;
    }

    m_engine.Process(
        const_cast<const float* const*>(inputs),
        inputChCount,
        static_cast<uint32_t>(sampleFrames),
        outputs);

    // Zero channels with no physical speakers (fallback channel-based path)
    ZeroUnusedChannels(outputs, sampleFrames, SpeakerLayoutFromParam());
}

InputLayout MagicSpatialVst::DetectLayoutFromInputs(float** inputs, VstInt32 sampleFrames) {
    auto hasSignal = [&](int channel) -> bool {
        if (channel >= kNumInputs) return false;
        float energy = 0.0f;
        int step = std::max(1, sampleFrames / 64);
        for (VstInt32 i = 0; i < sampleFrames; i += step) {
            energy += inputs[channel][i] * inputs[channel][i];
        }
        // Threshold must be well above E-APO residual noise on unused channels.
        // 1e-4 ≈ -40 dBFS — real audio signals are far above this.
        return energy > 1e-4f;
    };

    if (hasSignal(8) || hasSignal(9) || hasSignal(10) || hasSignal(11)) {
        return InputLayout::Passthrough;
    }
    if (hasSignal(6) || hasSignal(7)) {
        return InputLayout::Surround71;
    }
    if (hasSignal(2) || hasSignal(3) || hasSignal(4) || hasSignal(5)) {
        return InputLayout::Surround51;
    }
    return InputLayout::Stereo;
}

void MagicSpatialVst::UpdateEngine() {
    m_engineInitialized = false;
}

void MagicSpatialVst::EditorRedraw() {
    // No longer needed — controls handle themselves
}

void MagicSpatialVst::InitSpatialDsp() {
    if (m_spatialDspInitialized) return;

    float sr = m_sampleRate;
    uint32_t maxFrames = static_cast<uint32_t>(m_blockSize);

    m_spatialSeparator.Initialize(sr, maxFrames);
    m_spatialSplitter.Initialize(sr, maxFrames);
    m_spatialCorrelation.Initialize(sr);
    m_spatialTransients.Initialize(sr);
    m_spatialLfeLowpass.SetCoeffs(DesignLowpass(80.0f, sr));

    const auto* presets = GetDecorrelatorPresets();
    for (int i = 0; i < 8; ++i) {
        m_spatialDecorr[i].Initialize(presets[i].coefficients, 3, presets[i].delaySamples);
    }

    // Pre-allocate all scratch buffers (#1: no heap allocation on audio thread)
    for (int b = 0; b < 4; ++b) {
        m_sBandL[b].resize(maxFrames);
        m_sBandR[b].resize(maxFrames);
    }
    m_sSide2.resize(maxFrames); m_sSide3.resize(maxFrames);
    m_sFullMid.resize(maxFrames); m_sTransients.resize(maxFrames);
    m_sScratch.resize(maxFrames);
    m_sSurrL.resize(maxFrames); m_sSurrR.resize(maxFrames);
    m_sHeightL.resize(maxFrames); m_sHeightR.resize(maxFrames);
    m_sAmbL.resize(maxFrames); m_sAmbR.resize(maxFrames);
    m_sDelayedL.resize(maxFrames); m_sDelayedR.resize(maxFrames);
    m_sCenter.resize(maxFrames);
    m_sResidualL.resize(maxFrames); m_sResidualR.resize(maxFrames);

    m_spatialDspInitialized = true;
}

void MagicSpatialVst::ProcessSpatialObjects(float** inputs, float** outputs, VstInt32 sampleFrames) {
    InitSpatialDsp();

    const float* inL = inputs[0];
    const float* inR = inputs[1];
    uint32_t frames = static_cast<uint32_t>(sampleFrames);

    // --- Step 0: Frequency-domain separation ---
    // Per-bin phase/amplitude mask peels the phantom centre cleanly out of the
    // stereo field without touching overlapping neighbour frequencies. All
    // downstream analysis runs on the delayed signals so nothing desyncs.
    m_spatialSeparator.Process(inL, inR, frames,
                               m_sDelayedL.data(), m_sDelayedR.data(), m_sCenter.data());
    const float* L = m_sDelayedL.data();
    const float* R = m_sDelayedR.data();
    const float* C = m_sCenter.data();

    // --- Residuals = delayed L/R minus spectral centre ---
    // Every surround/height/front-L-R feed draws from these so vocals never
    // leak into anything but OBJ_VOCAL (and a little into the sub below).
    for (uint32_t i = 0; i < frames; ++i) {
        m_sResidualL[i] = L[i] - C[i];
        m_sResidualR[i] = R[i] - C[i];
    }
    const float* rL = m_sResidualL.data();
    const float* rR = m_sResidualR.data();

    // --- Multiband split on RESIDUALS ---
    float* bandL[4] = {m_sBandL[0].data(), m_sBandL[1].data(), m_sBandL[2].data(), m_sBandL[3].data()};
    float* bandR[4] = {m_sBandR[0].data(), m_sBandR[1].data(), m_sBandR[2].data(), m_sBandR[3].data()};
    m_spatialSplitter.Process(rL, rR, frames, bandL, bandR);

    // Aliases for readability (residual bands)
    float* bL1 = bandL[1]; float* bL2 = bandL[2]; float* bL3 = bandL[3];
    float* bR1 = bandR[1]; float* bR2 = bandR[2]; float* bR3 = bandR[3];

    // --- Per-band correlation ---
    float corr[4];
    for (int b = 0; b < 4; ++b) {
        corr[b] = m_spatialCorrelation.ProcessBand(b, bandL[b], bandR[b], frames);
    }

    // Smoothed surround weights per band: linear mapping of correlation to a
    // [0..1] gate. No squaring — the SpectralSeparator already peels centre
    // content out at the source, so aggressive vocal-rejection on top of that
    // was costing the surrounds ~6 dB for no added benefit. The linear sw
    // still acts as a gentle defensive floor against any centre energy that
    // slips past the spectral mask.
    float sw[4];
    for (int b = 0; b < 4; ++b) {
        float target = 1.0f - (corr[b] + 1.0f) * 0.5f;
        m_smoothSW[b] += kSWSmoothing * (target - m_smoothSW[b]);
        sw[b] = m_smoothSW[b];
    }

    // --- Transient detection ---
    for (uint32_t i = 0; i < frames; ++i) {
        m_sFullMid[i] = (L[i] + R[i]) * 0.5f;
    }
    m_spatialTransients.Process(m_sFullMid.data(), m_sTransients.data(), frames);

    // --- Per-band sides (residual bands -> already centre-free) ---
    for (uint32_t i = 0; i < frames; ++i) {
        m_sSide2[i] = (bL2[i] - bR2[i]) * 0.5f;
        m_sSide3[i] = (bL3[i] - bR3[i]) * 0.5f;
    }

    // Dynamic L/R position steering: compute energy balance on the delayed mid
    // range and slide the L/R objects slightly toward the dominant side. Base
    // angles are the ITU reference ±30° positions; the balance adds at most
    // ±0.08 of horizontal sway so the pair never collapses.
    float energyL = 0.0f, energyR = 0.0f;
    int step = std::max(1, sampleFrames / 64);
    for (VstInt32 i = 0; i < sampleFrames; i += step) {
        energyL += L[i] * L[i];
        energyR += R[i] * R[i];
    }
    float totalEnergy = energyL + energyR + 1e-10f;
    float rawBalance = (energyR - energyL) / totalEnergy; // -1 = all left, +1 = all right

    // One-pole smoothing so sudden panning (e.g. a hard-left effect) slides
    // the front pair across the stage rather than teleporting.
    m_smoothBalance += kBalanceSmoothing * (rawBalance - m_smoothBalance);

    float leftX  = -0.500f + m_smoothBalance * 0.08f;
    float rightX =  0.500f + m_smoothBalance * 0.08f;
    m_spatialWriter.SetObjectPosition(SpatialObjectWriter::OBJ_LEFT,  leftX,  0.0f, -0.866f);
    m_spatialWriter.SetObjectPosition(SpatialObjectWriter::OBJ_RIGHT, rightX, 0.0f, -0.866f);

    // Vocal head-lock with pitch-tracked elevation.
    //
    // Azimuth is rock-locked at 0° (centre image never wobbles). Elevation
    // instead tracks a rough fundamental-frequency estimate derived from the
    // zero-crossing rate of the spectral centre stream: low voices (narrators,
    // chest voice) sit at y=0; high voices (soprano, head voice, emphatic
    // dialogue) float slightly upward.  The distance is pulled in to z=-0.7
    // for an intimate "head in the room" feel, matching how Dolby encodes
    // dialogue objects in mixed content.
    //
    // The pitch update is energy-gated: during silence, m_smoothVocalPitch
    // holds its previous value rather than drifting back to zero, so the
    // very first syllable after a pause enters at a sensible elevation.
    {
        float centerEnergy = 0.0f;
        int crossings = 0;
        float prev = 0.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            float s = C[i];
            centerEnergy += s * s;
            if ((s >= 0.0f) != (prev >= 0.0f)) ++crossings;
            prev = s;
        }

        // Gate: require at least -60 dBFS average level before trusting the ZCR.
        float meanEnergy = centerEnergy / static_cast<float>(frames);
        if (meanEnergy > 1e-6f) {
            // ZCR ≈ 2·f0 for a mostly-periodic signal. Convert to Hz and
            // divide by 2 to estimate fundamental.
            float zcr = static_cast<float>(crossings) * m_sampleRate
                      / static_cast<float>(frames);
            float estF0 = zcr * 0.5f;

            // Map [100..400] Hz → [0..1]. Below 100 Hz treats as low voice,
            // above 400 Hz as full soprano.
            float target = (estF0 - 100.0f) / 300.0f;
            if (target < 0.0f) target = 0.0f;
            if (target > 1.0f) target = 1.0f;

            m_smoothVocalPitch += kPitchSmoothing * (target - m_smoothVocalPitch);
        }

        // Subtle elevation swing up to +0.15 units, kept small so dialogue
        // never detaches from the front image.
        float vy = m_smoothVocalPitch * 0.15f;
        m_spatialWriter.SetObjectPosition(SpatialObjectWriter::OBJ_VOCAL, 0.0f, vy, -0.7f);
    }

    // Dynamic height elevation via the Pratt effect: bright residual content
    // floats all four overhead objects upward; dark/bass-heavy content settles
    // them back. Brightness is computed from residual band energies (band 3 /
    // total), smoothed at block rate, and mapped to an elevation angle.
    // Top-front and top-back share the same elevation but have different
    // azimuths (±30° front, ±150° back).
    float hy, hxMag, hzFront, hzBack;
    {
        float e0 = 0.0f, e1 = 0.0f, e2 = 0.0f, e3 = 0.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            e0 += bandL[0][i] * bandL[0][i] + bandR[0][i] * bandR[0][i];
            e1 += bandL[1][i] * bandL[1][i] + bandR[1][i] * bandR[1][i];
            e2 += bandL[2][i] * bandL[2][i] + bandR[2][i] * bandR[2][i];
            e3 += bandL[3][i] * bandL[3][i] + bandR[3][i] * bandR[3][i];
        }
        float total = e0 + e1 + e2 + e3 + 1e-10f;
        float rawBrightness = e3 / total;

        float target = std::min(1.0f, rawBrightness * 4.0f);
        m_smoothHeightBrightness += kBrightnessSmoothing * (target - m_smoothHeightBrightness);

        constexpr float kPi = 3.14159265f;
        constexpr float kMinElevRad = 35.0f * kPi / 180.0f;
        constexpr float kMaxElevRad = 70.0f * kPi / 180.0f;
        float elev = kMinElevRad + m_smoothHeightBrightness * (kMaxElevRad - kMinElevRad);
        hy = std::sin(elev);
        float horiz = std::cos(elev);
        constexpr float kSin30 = 0.5f;
        constexpr float kCos30 = 0.8660254f;
        hxMag   = horiz * kSin30;
        hzFront = -horiz * kCos30;  // negative Z = forward
        hzBack  =  horiz * kCos30;  // positive Z = behind

        m_spatialWriter.SetObjectPosition(SpatialObjectWriter::OBJ_TOP_FRONT_L, -hxMag, hy, hzFront);
        m_spatialWriter.SetObjectPosition(SpatialObjectWriter::OBJ_TOP_FRONT_R, +hxMag, hy, hzFront);
        m_spatialWriter.SetObjectPosition(SpatialObjectWriter::OBJ_TOP_BACK_L,  -hxMag, hy, hzBack);
        m_spatialWriter.SetObjectPosition(SpatialObjectWriter::OBJ_TOP_BACK_R,  +hxMag, hy, hzBack);
    }

    // --- OBJ_SUBBASS: 80 Hz lowpass of the DELAYED ORIGINAL mid ---
    m_spatialLfeLowpass.Process(m_sFullMid.data(), m_sScratch.data(), frames);
    m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_SUBBASS, m_sScratch.data(), frames);

    // --- OBJ_VOCAL: per-bin spectral centre, directly ---
    m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_VOCAL, C, frames);

    // --- OBJ_LEFT / OBJ_RIGHT: residuals (delayed L/R minus spectral centre) ---
    m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_LEFT, rL, frames);
    m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_RIGHT, rR, frames);

    // ================================================================
    // SURROUND SPLIT: each frequency band feeds exactly ONE object pair.
    // Band 1 + band 3 → SIDES (±90°), band 2 → BACKS (±135°).
    // No audio content is duplicated — zero echo risk.
    // ================================================================

    // --- OBJ_SIDE_LEFT/RIGHT: band 1 (room warmth) + band 3 (treble shimmer) ---
    {
        std::memset(m_sSurrL.data(), 0, frames * sizeof(float));
        std::memset(m_sSurrR.data(), 0, frames * sizeof(float));

        // Band 1 low-mid side through decorrelators [0, 1]
        {
            for (uint32_t i = 0; i < frames; ++i) {
                float sideL = (bL1[i] - bR1[i]) * 0.5f;
                m_sAmbL[i] =  sideL * sw[1];
                m_sAmbR[i] = -sideL * sw[1];
            }
            m_spatialDecorr[0].Process(m_sAmbL.data(), m_sScratch.data(), frames);
            for (uint32_t i = 0; i < frames; ++i) {
                float tDuck = 1.0f - m_sTransients[i] * 0.5f;
                m_sSurrL[i] += m_sScratch[i] * 1.60f * tDuck;
            }
            m_spatialDecorr[1].Process(m_sAmbR.data(), m_sScratch.data(), frames);
            for (uint32_t i = 0; i < frames; ++i) {
                float tDuck = 1.0f - m_sTransients[i] * 0.5f;
                m_sSurrR[i] += m_sScratch[i] * 1.60f * tDuck;
            }
        }

        // Band 3 treble side (direct, no decorrelator — inverted for width)
        for (uint32_t i = 0; i < frames; ++i) {
            m_sSurrL[i] += m_sSide3[i] * 1.75f * sw[3];
            m_sSurrR[i] -= m_sSide3[i] * 1.75f * sw[3];
        }

        m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_SIDE_LEFT, m_sSurrL.data(), frames);
        m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_SIDE_RIGHT, m_sSurrR.data(), frames);
    }

    // --- OBJ_BACK_LEFT/RIGHT: band 2 (presence depth, 2k-8k) ---
    // Reuses m_sSurrL/R (safe — SubmitObjectAudio already copied the side data).
    {
        m_spatialDecorr[2].Process(m_sSide2.data(), m_sScratch.data(), frames);
        for (uint32_t i = 0; i < frames; ++i) {
            float tDuck = 1.0f - m_sTransients[i] * 0.5f;
            m_sSurrL[i] = m_sScratch[i] * 2.50f * sw[2] * tDuck;
        }
        m_spatialDecorr[3].Process(m_sSide2.data(), m_sScratch.data(), frames);
        for (uint32_t i = 0; i < frames; ++i) {
            float tDuck = 1.0f - m_sTransients[i] * 0.5f;
            m_sSurrR[i] = m_sScratch[i] * 2.50f * sw[2] * tDuck;
        }

        m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_BACK_LEFT, m_sSurrL.data(), frames);
        m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_BACK_RIGHT, m_sSurrR.data(), frames);
    }

    // ================================================================
    // HEIGHT SPLIT: band 2 mid → top-front, band 3 direct → top-back.
    // Band 2 feeds BACKS as side (L-R)/2 and TOP_FRONT as mid (L+R)/2 —
    // these are mathematically orthogonal, so zero comb filtering.
    // Band 3 feeds TOP_BACK only (different frequency entirely).
    // ================================================================

    // --- OBJ_TOP_FRONT_L/R: band 2 residual mono mid, decorrelated ---
    // Using mid = (bL2+bR2)/2 makes this orthogonal to the BACK feed which
    // uses side = (bL2-bR2)/2. Both decorrelator outputs are distinct because
    // presets [4,5] differ from [2,3].
    {
        for (uint32_t i = 0; i < frames; ++i) {
            float mid2 = (bL2[i] + bR2[i]) * 0.5f;
            float tDuck = 1.0f - m_sTransients[i] * 0.5f;
            m_sAmbL[i] = mid2 * 2.00f * tDuck;
            m_sAmbR[i] = mid2 * 2.00f * tDuck;
        }

        m_spatialDecorr[4].Process(m_sAmbL.data(), m_sHeightL.data(), frames);
        m_spatialDecorr[5].Process(m_sAmbR.data(), m_sHeightR.data(), frames);

        m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_TOP_FRONT_L, m_sHeightL.data(), frames);
        m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_TOP_FRONT_R, m_sHeightR.data(), frames);
    }

    // --- OBJ_TOP_BACK_L/R: band 3 residual direct, decorrelated ---
    // Entirely different frequency band (>8k) from top-front (2k-8k). Zero
    // content overlap. Decorrelator presets [6,7] provide distinct diffusion.
    {
        for (uint32_t i = 0; i < frames; ++i) {
            float tDuck = 1.0f - m_sTransients[i] * 0.5f;
            m_sAmbL[i] = bL3[i] * 2.80f * tDuck;
            m_sAmbR[i] = bR3[i] * 2.80f * tDuck;
        }

        m_spatialDecorr[6].Process(m_sAmbL.data(), m_sHeightL.data(), frames);
        m_spatialDecorr[7].Process(m_sAmbR.data(), m_sHeightR.data(), frames);

        m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_TOP_BACK_L, m_sHeightL.data(), frames);
        m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_TOP_BACK_R, m_sHeightR.data(), frames);
    }

    // Write original L/R to channel outputs as fallback. The caller
    // (ProcessReplacing) will zero these if the spatial writer is active.
    std::memcpy(outputs[CH_FL], L, sampleFrames * sizeof(float));
    std::memcpy(outputs[CH_FR], R, sampleFrames * sizeof(float));
    for (int ch = CH_C; ch < kAtmosChannelCount; ++ch) {
        std::memset(outputs[ch], 0, sampleFrames * sizeof(float));
    }
}

} // namespace MagicSpatial
