#include "vst/MagicSpatialVst.h"
#include "core/Log.h"

#include <windows.h>
#include <commctrl.h>
#include <immintrin.h>  // _mm_setcsr / _mm_getcsr for FTZ/DAZ
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace MagicSpatial {


// --- Editor control IDs ---
enum EditorCtrlID {
    ID_MODE_COMBO         = 1001,
    ID_SPEAKERS_COMBO     = 1002,
    ID_SURROUND_POS_LABEL = 1003,
    ID_SURROUND_POS_COMBO = 1004,
};

// Helper: show or hide the SurroundPos combo + label based on the current
// speakers parameter. Only relevant for 5.1 / 5.1.2 / 5.1.4 layouts where the
// physical surround speaker placement might be at the back rather than the side.
static void UpdateSurroundPosVisibility(HWND parent, float speakersParam) {
    bool isFiveOne = (speakersParam >= 0.075f) && (speakersParam < 0.525f); // 5.1, 5.1.2, 5.1.4
    int show = isFiveOne ? SW_SHOW : SW_HIDE;
    if (HWND lbl   = GetDlgItem(parent, ID_SURROUND_POS_LABEL)) ShowWindow(lbl,   show);
    if (HWND combo = GetDlgItem(parent, ID_SURROUND_POS_COMBO)) ShowWindow(combo, show);
}

// --- Editor WndProc ---
static LRESULT CALLBACK EditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* plugin = reinterpret_cast<MagicSpatialVst*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_COMMAND:
        if (!plugin) break;
        if (HIWORD(wParam) == CBN_SELCHANGE) {
            int id = LOWORD(wParam);
            int sel = static_cast<int>(SendMessageW(reinterpret_cast<HWND>(lParam), CB_GETCURSEL, 0, 0));
            // Combo changes write directly to the live parameter members
            // (immediate audio effect). We deliberately do NOT call
            // audioMasterAutomate — it triggers E-APO to reload the plugin
            // and the new instance fails to recover the spatial audio stream
            // (the plugin stops rendering until the user toggles the filter
            // off and on). Tradeoff: E-APO's outer Cancel button cannot
            // revert session changes. Persistence across E-APO restarts
            // still works via effGetChunk/effSetChunk.
            if (id == ID_MODE_COMBO) {
                plugin->m_paramMode = static_cast<float>(sel) * 0.25f;
                plugin->m_engineInitialized = false;
            }
            else if (id == ID_SPEAKERS_COMBO) {
                // Spacing 0.15 per slot keeps decoder midpoints clean.
                plugin->m_paramSpeakers = static_cast<float>(sel) * 0.15f;
                UpdateSurroundPosVisibility(hwnd, plugin->m_paramSpeakers);
            }
            else if (id == ID_SURROUND_POS_COMBO) {
                // sel: 0 = Side, 1 = Rear
                plugin->m_paramSurroundPos = (sel == 0) ? 0.0f : 1.0f;
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
        // Mark BOTH the channel engine and the spatial DSP dirty so they
        // re-initialize with the new rate on the next audio block. (Most
        // hosts call this only once at startup, but some do change it
        // mid-session.)
        m_engineInitialized = false;
        m_spatialDspInitialized = false;
        return 0;

    case effSetBlockSize:
        m_blockSize = static_cast<VstInt32>(value);
        m_engineInitialized = false;
        m_spatialDspInitialized = false;
        // Pre-zero the silence buffer used by ProcessMultichannelObjects to
        // submit silence to OBJ_* slots that a given input layout doesn't feed.
        m_silenceBuffer.assign(static_cast<size_t>(m_blockSize), 0.0f);
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
        SendMessageW(spkCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"5.1"));
        SendMessageW(spkCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"5.1.2"));
        SendMessageW(spkCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"5.1.4"));
        SendMessageW(spkCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"7.1"));
        SendMessageW(spkCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"7.1.2"));
        SendMessageW(spkCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"7.1.4"));
        int spkSel = (m_paramSpeakers < 0.075f) ? 0 :
                     (m_paramSpeakers < 0.225f) ? 1 :
                     (m_paramSpeakers < 0.375f) ? 2 :
                     (m_paramSpeakers < 0.525f) ? 3 :
                     (m_paramSpeakers < 0.675f) ? 4 :
                     (m_paramSpeakers < 0.825f) ? 5 : 6;
        SendMessageW(spkCombo, CB_SETCURSEL, spkSel, 0);
        y += 30;

        // --- Surround Position combo (only shown for 5.1 / 5.1.2 / 5.1.4) ---
        // Lets the user tell the plugin that their physical surround speakers
        // are placed behind them rather than at the sides — common mismatch
        // with the ITU reference layout. When set to "Rear", OBJ_SIDE_LEFT/RIGHT
        // are positioned at ±135° so the Dolby renderer maps side content onto
        // the user's physical rear speakers.
        CreateWindowW(L"STATIC", L"Surround:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, y + 2, labelW, 20, hwnd,
            reinterpret_cast<HMENU>(ID_SURROUND_POS_LABEL), hInst, nullptr);
        HWND surrPosCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            ctrlX, y, ctrlW, 100, hwnd,
            reinterpret_cast<HMENU>(ID_SURROUND_POS_COMBO), hInst, nullptr);
        SendMessageW(surrPosCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Side (90\u00B0)"));
        SendMessageW(surrPosCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Rear (135\u00B0)"));
        SendMessageW(surrPosCombo, CB_SETCURSEL,
                     (m_paramSurroundPos < 0.5f) ? 0 : 1, 0);

        // Match initial visibility to the speakers selection.
        UpdateSurroundPosVisibility(hwnd, m_paramSpeakers);

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
            case 0: std::strncpy(buf, "Mode",        kVstMaxParamStrLen); break;
            case 1: std::strncpy(buf, "Speakers",    kVstMaxParamStrLen); break;
            case 2: std::strncpy(buf, "SurroundPos", kVstMaxParamStrLen); break;
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
                if      (m_paramSpeakers < 0.075f) std::strncpy(buf, "2.0",   kVstMaxParamStrLen);
                else if (m_paramSpeakers < 0.225f) std::strncpy(buf, "5.1",   kVstMaxParamStrLen);
                else if (m_paramSpeakers < 0.375f) std::strncpy(buf, "5.1.2", kVstMaxParamStrLen);
                else if (m_paramSpeakers < 0.525f) std::strncpy(buf, "5.1.4", kVstMaxParamStrLen);
                else if (m_paramSpeakers < 0.675f) std::strncpy(buf, "7.1",   kVstMaxParamStrLen);
                else if (m_paramSpeakers < 0.825f) std::strncpy(buf, "7.1.2", kVstMaxParamStrLen);
                else                               std::strncpy(buf, "7.1.4", kVstMaxParamStrLen);
                break;
            case 2:
                if (m_paramSurroundPos < 0.5f) std::strncpy(buf, "Side", kVstMaxParamStrLen);
                else                           std::strncpy(buf, "Rear", kVstMaxParamStrLen);
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
    case 2:
        m_paramSurroundPos = value;
        break;
    }
}

float MagicSpatialVst::GetParameter(VstInt32 index) {
    switch (index) {
    case 0: return m_paramMode;
    case 1: return m_paramSpeakers;
    case 2: return m_paramSurroundPos;
    default: return 0.0f;
    }
}



// --- Processing ---

void MagicSpatialVst::ApplySurroundPositionOverride() {
    // Only applies for 5.1 / 5.1.2 / 5.1.4 layouts where the user might have
    // physical surrounds at the back rather than the side. For 7.1.x layouts
    // the user has both side AND back physical speakers, so the OBJ_SIDE_*
    // objects rightly belong at ±90° (default).
    SpeakerLayout sl = SpeakerLayoutFromParam();
    bool isFiveOne = (sl == SpeakerLayout::Layout_51 ||
                      sl == SpeakerLayout::Layout_512 ||
                      sl == SpeakerLayout::Layout_514);
    bool wantsRear = (m_paramSurroundPos >= 0.5f);
    if (isFiveOne && wantsRear) {
        // Move OBJ_SIDE_* to the back-surround coordinates so the Dolby
        // renderer routes "side" content onto the user's physical rear pair.
        m_spatialWriter.SetObjectPosition(SpatialObjectWriter::OBJ_SIDE_LEFT,  -0.707f, 0.0f, 0.707f);
        m_spatialWriter.SetObjectPosition(SpatialObjectWriter::OBJ_SIDE_RIGHT,  0.707f, 0.0f, 0.707f);
    }
    // Else: leave at whatever defaults were last applied (±1.0, 0, 0 by ITU).
}

SpeakerLayout MagicSpatialVst::SpeakerLayoutFromParam() const {
    // Combo encoding: sel * 0.15 → 0.0, 0.15, 0.30, 0.45, 0.60, 0.75, 0.90.
    // Decode at the midpoints.
    if      (m_paramSpeakers < 0.075f) return SpeakerLayout::Layout_20;
    else if (m_paramSpeakers < 0.225f) return SpeakerLayout::Layout_51;
    else if (m_paramSpeakers < 0.375f) return SpeakerLayout::Layout_512;
    else if (m_paramSpeakers < 0.525f) return SpeakerLayout::Layout_514;
    else if (m_paramSpeakers < 0.675f) return SpeakerLayout::Layout_71;
    else if (m_paramSpeakers < 0.825f) return SpeakerLayout::Layout_712;
    else                               return SpeakerLayout::Layout_714;
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
    case SpeakerLayout::Layout_51:
        zeroChannel(CH_BL);
        zeroChannel(CH_BR);
        zeroChannel(CH_TFL);
        zeroChannel(CH_TFR);
        zeroChannel(CH_TBL);
        zeroChannel(CH_TBR);
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
    case SpeakerLayout::Layout_71:
        zeroChannel(CH_TFL);
        zeroChannel(CH_TFR);
        zeroChannel(CH_TBL);
        zeroChannel(CH_TBR);
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

    // --- Crackle defenses on audio-thread entry ---
    //
    // 1) Enable flush-to-zero (FTZ) and denormals-are-zero (DAZ) on the SSE
    //    control register. Our IIR-heavy pipeline (correlation/transient/
    //    surround-weight/balance/brightness/pitch smoothers, decorrelator
    //    allpass cascades, biquad lowpass) can decay state values into
    //    subnormal float range during quiet passages. Subnormals stall the
    //    CPU by orders of magnitude, which causes the audio thread to miss
    //    deadlines and produces random per-session crackling. Setting this
    //    once per block is enough — the bits persist for the thread.
    _mm_setcsr(_mm_getcsr() | 0x8040u);

    // 2) Defensively sanitize inputs: replace any NaN/Inf samples with 0.
    //    A misbehaving upstream (or a glitched device) can inject non-finite
    //    floats which would then poison every IIR filter state in our chain
    //    permanently for the session. We modify in place since VST2 input
    //    buffers are owned by the host per-block and not re-read after we
    //    return. Cost ~12·blockSize cheap branches per block — negligible.
    for (int ch = 0; ch < kNumInputs; ++ch) {
        float* in = inputs[ch];
        if (!in) continue;
        for (VstInt32 i = 0; i < sampleFrames; ++i) {
            if (!std::isfinite(in[i])) in[i] = 0.0f;
        }
    }

    // Detect input layout first — needed to decide spatial vs channel path.
    // In Auto mode, apply hysteresis: the committed layout only changes after
    // the raw detection has consistently reported a new value for N blocks.
    // This absorbs transient silences and prevents rapid ping-pong between
    // the stereo DSP path and the multichannel object path, which would
    // otherwise cause DSP-state discontinuities audible as crackling.
    InputLayout targetLayout;
    if (m_paramMode < 0.125f) {
        InputLayout raw = DetectLayoutFromInputs(inputs, sampleFrames);
        if (raw == m_committedLayout) {
            // Raw matches the committed layout — reset any pending change.
            m_pendingLayout = raw;
            m_pendingCount = 0;
        } else if (raw == m_pendingLayout) {
            // Consistent with what we're watching — accumulate evidence.
            if (++m_pendingCount >= kLayoutHysteresisBlocks) {
                m_committedLayout = m_pendingLayout;
                m_pendingCount = 0;
            }
        } else {
            // A third value appeared — reset and start counting from this one.
            m_pendingLayout = raw;
            m_pendingCount = 1;
        }
        targetLayout = m_committedLayout;
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

    // --- Spatial-object mode dispatch ---
    //
    // Two object-based paths exist. The stereo path runs heavy DSP (spectral
    // separator + multiband + decorrelation) and is gated independently so it
    // can also fall back to channel output during writer warmup. The
    // multichannel path is a zero-DSP channel→object promotion that only
    // engages when the writer is active; otherwise it falls through to the
    // existing m_engine channel path below.

    // 1) Stereo input → spatial DSP path (always enters in Auto/Stereo mode;
    //    has its own internal channel-output fallback for writer warmup).
    if (targetLayout == InputLayout::Stereo &&
        (m_paramMode < 0.125f || m_paramMode < 0.375f)) { // Auto or Stereo
        ProcessSpatialObjects(inputs, outputs, sampleFrames);

        if (m_spatialWriter.IsActive()) {
            for (int ch = 0; ch < kAtmosChannelCount; ++ch) {
                std::memset(outputs[ch], 0, sampleFrames * sizeof(float));
            }
        }
        return;
    }

    // 2) Multichannel input → object-promotion path (only when writer ready).
    //    Each channel becomes a positioned Atmos object at its ITU reference
    //    position. Zero added latency, zero remixing — the Dolby renderer
    //    handles physical-speaker mapping. Falls through to m_engine if the
    //    writer is inactive.
    if ((targetLayout == InputLayout::Surround51 ||
         targetLayout == InputLayout::Surround71 ||
         targetLayout == InputLayout::Passthrough) &&
        m_spatialWriter.IsActive()) {
        ProcessMultichannelObjects(inputs, outputs, sampleFrames, targetLayout);
        for (int ch = 0; ch < kAtmosChannelCount; ++ch) {
            std::memset(outputs[ch], 0, sampleFrames * sizeof(float));
        }
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
    m_sSide0.resize(maxFrames); m_sSide1.resize(maxFrames);
    m_sSide2.resize(maxFrames); m_sSide3.resize(maxFrames);
    m_sFullMid.resize(maxFrames); m_sTransients.resize(maxFrames);
    m_sScratch.resize(maxFrames);
    m_sSurrL.resize(maxFrames); m_sSurrR.resize(maxFrames);
    m_sHeightL.resize(maxFrames); m_sHeightR.resize(maxFrames);
    m_sAmbL.resize(maxFrames); m_sAmbR.resize(maxFrames);
    m_sDelayedL.resize(maxFrames); m_sDelayedR.resize(maxFrames);
    m_sCenter.resize(maxFrames);
    m_sResidualL.resize(maxFrames); m_sResidualR.resize(maxFrames);

    // Delay rings + scratch for stereo-aware multichannel extraction.
    // Only channels 2..11 need delaying; FL/FR (0/1) come out of the
    // SpectralSeparator already kFftSize-delayed.
    for (int ch = 2; ch < kNumInputs; ++ch) {
        m_mcDelayRing[ch].assign(kMcDelaySize, 0.0f);
        m_mcDelayed[ch].resize(maxFrames);
    }
    m_mcDelayPos = 0;

    // --- Feature 1: Early-reflection pre-delay (12 ms) ---
    m_rearDelayLength = static_cast<int>(sr * static_cast<float>(kRearPreDelayMs) / 1000.0f);
    if (m_rearDelayLength > kRearPreDelayMaxSamples)
        m_rearDelayLength = kRearPreDelayMaxSamples;
    if (m_rearDelayLength < 1)
        m_rearDelayLength = 1;
    for (int i = 0; i < 4; ++i)
        m_rearDelayRing[i].assign(m_rearDelayLength, 0.0f);
    m_rearDelaySidePos = 0;
    m_rearDelayBackPos = 0;

    // --- Feature 4: Feedback diffusion ---
    for (int i = 0; i < 4; ++i)
        m_rearDiffuser[i].Initialize(sr);

    // --- Feature 3: Ambience bin counts per band ---
    {
        float binHz = sr / static_cast<float>(SpectralSeparator::kFftSize);
        for (int b = 0; b < 4; ++b) m_ambBinCount[b] = 0;
        for (int k = 0; k < SpectralSeparator::kNumBins; ++k) {
            float freq = static_cast<float>(k) * binHz;
            int band = (freq < 200.0f) ? 0 : (freq < 2000.0f) ? 1 : (freq < 8000.0f) ? 2 : 3;
            m_ambBinCount[band]++;
        }
    }

    m_spatialDspInitialized = true;
}

void MagicSpatialVst::ProcessSpatialObjects(float** inputs, float** outputs, VstInt32 sampleFrames) {
    InitSpatialDsp();

    // Apply user's surround-position override (Side vs Rear). Cheap (2
    // SetObjectPosition calls when the override is active, zero calls when
    // not) and ensures consistency with the multichannel path.
    ApplySurroundPositionOverride();

    // Feature 2: Correlation-adaptive spatial extension gain (replaces fixed
    // 0.60). Computed after per-band correlation below — declared here so it
    // is in scope for all four submit sites.
    float spatialExtGain = 0.60f; // overwritten below after corr[] is computed

    const float* inL = inputs[0];
    const float* inR = inputs[1];
    uint32_t frames = static_cast<uint32_t>(sampleFrames);

    // --- Step 0: Frequency-domain separation ---
    // Per-bin phase/amplitude mask peels the phantom centre cleanly out of the
    // stereo field without touching overlapping neighbour frequencies. All
    // downstream analysis runs on the delayed signals so nothing desyncs.
    m_spatialSeparator.Process(inL, inR, frames,
                               m_sDelayedL.data(), m_sDelayedR.data(), m_sCenter.data());

    // Global output normalization: scale all three separator outputs so the
    // total acoustic energy from ~8 active speakers matches the 2-speaker
    // bypass level. Applied here at the source so every downstream path
    // (residuals, band splits, side signals, all objects) inherits the
    // attenuation automatically. Analysis passes (correlation, transients,
    // brightness, pitch) all use energy ratios and are unaffected.
    constexpr float kGlobalOutputGain = 0.65f;
    for (uint32_t i = 0; i < frames; ++i) {
        m_sDelayedL[i] *= kGlobalOutputGain;
        m_sDelayedR[i] *= kGlobalOutputGain;
        m_sCenter[i]   *= kGlobalOutputGain;
    }

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
    float* bL0 = bandL[0]; float* bL1 = bandL[1]; float* bL2 = bandL[2]; float* bL3 = bandL[3];
    float* bR0 = bandR[0]; float* bR1 = bandR[1]; float* bR2 = bandR[2]; float* bR3 = bandR[3];

    // --- Per-band correlation ---
    float corr[4];
    for (int b = 0; b < 4; ++b) {
        corr[b] = m_spatialCorrelation.ProcessBand(b, bandL[b], bandR[b], frames);
    }

    // Smoothed surround weights per band: correlation-driven attenuation
    // floored at 0.5 so highly-correlated music (vocals + centred instruments)
    // still drives the surrounds at a useful level. The previous unfloored
    // mapping collapsed sw to ~0.15 for typical pop/rock and stacked on top
    // of the already-quiet side signal, leaving the rear pair barely audible.
    // The SpectralSeparator already peels the phantom centre out at the
    // source, so the gate's only remaining job is to soften any leak — a
    // half-strength floor is plenty.
    float sw[4];
    for (int b = 0; b < 4; ++b) {
        float target = 1.0f - (corr[b] + 1.0f) * 0.5f; // 0..1
        if (target < 0.5f) target = 0.5f;              // floor: never below half
        m_smoothSW[b] += kSWSmoothing * (target - m_smoothSW[b]);
        sw[b] = m_smoothSW[b];
    }

    // --- Transient detection ---
    for (uint32_t i = 0; i < frames; ++i) {
        m_sFullMid[i] = (L[i] + R[i]) * 0.5f;
    }
    m_spatialTransients.Process(m_sFullMid.data(), m_sTransients.data(), frames);

    // --- Feature 2: Correlation-adaptive spatial extension gain ---
    // Average correlation across bands (weighted by perceptual importance) to
    // modulate how aggressively spatial content wraps the listener. Reverberant
    // recordings expand; dry studio mixes stay intimate.
    {
        float avgCorr = 0.25f * corr[0] + 0.35f * corr[1]
                      + 0.25f * corr[2] + 0.15f * corr[3];
        float targetGain = kSpatialExtGainMax
                         - (avgCorr + 1.0f) * 0.5f * (kSpatialExtGainMax - kSpatialExtGainMin);
        if (targetGain < kSpatialExtGainMin) targetGain = kSpatialExtGainMin;
        if (targetGain > kSpatialExtGainMax) targetGain = kSpatialExtGainMax;
        m_smoothSpatialExtGain += kSpatialExtGainSmoothing * (targetGain - m_smoothSpatialExtGain);
        spatialExtGain = m_smoothSpatialExtGain;
    }

    // --- Per-band sides (residual bands -> already centre-free) ---
    // All four bands precomputed so each spatial object can carry the full
    // bandwidth (and Dolby's bass management can then route low frequencies
    // to LFE / capable speakers per the user's physical layout).
    for (uint32_t i = 0; i < frames; ++i) {
        m_sSide0[i] = (bL0[i] - bR0[i]) * 0.5f;
        m_sSide1[i] = (bL1[i] - bR1[i]) * 0.5f;
        m_sSide2[i] = (bL2[i] - bR2[i]) * 0.5f;
        m_sSide3[i] = (bL3[i] - bR3[i]) * 0.5f;
    }

    // --- Feature 3: Per-band ambience factors from spectral variance ---
    float ambFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    {
        const float* ambW = m_spatialSeparator.GetAmbienceWeights();
        float binHz = m_sampleRate / static_cast<float>(SpectralSeparator::kFftSize);
        float ambSum[4] = {0.f, 0.f, 0.f, 0.f};
        for (int k = 0; k < SpectralSeparator::kNumBins; ++k) {
            float freq = static_cast<float>(k) * binHz;
            int band = (freq < 200.0f) ? 0 : (freq < 2000.0f) ? 1 : (freq < 8000.0f) ? 2 : 3;
            ambSum[band] += ambW[k];
        }
        for (int b = 0; b < 4; ++b) {
            float avg = ambSum[b] / static_cast<float>(m_ambBinCount[b] > 0 ? m_ambBinCount[b] : 1);
            // Map ambience [0,1] to a boost factor [1.0, 1.3]: high ambience
            // → up to +30% more routing to spatial extension objects.
            float target = 1.0f + avg * 0.3f;
            m_smoothAmbFactor[b] += kAmbFactorSmoothing * (target - m_smoothAmbFactor[b]);
            ambFactor[b] = m_smoothAmbFactor[b];
        }
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

    // Layout-aware: when no physical top-back exists (everything other than
    // 5.1.4 / 7.1.4) the top-back side blend gets folded TWO ways — into the
    // top-front feed (already there) AND into the surround feed below at 50%
    // through decorrelators [6]/[7]. The latter reinforces the
    // "behind-and-above" perceptual cue: the user's rear physical pair carries
    // a phase-scrambled echo of what would have come from the missing rear
    // ceiling speakers. Decorrelators [6]/[7] are otherwise unused in *.1.2
    // layouts (they normally drive OBJ_TOP_BACK), so we appropriate them here.
    SpeakerLayout sl_h = SpeakerLayoutFromParam();
    const bool hasTopBack = (sl_h == SpeakerLayout::Layout_514 ||
                             sl_h == SpeakerLayout::Layout_714);

    // --- OBJ_SIDE_LEFT/RIGHT: full-bandwidth side signal ---
    // Decorrelated band 1 + direct band 3 form the spectral character
    // (room warmth + treble shimmer). Bands 0 and 2 are added direct at
    // modest gains so the object carries all four bands and Dolby's bass
    // management can decide where low frequencies go (LFE vs. mains).
    {
        std::memset(m_sSurrL.data(), 0, frames * sizeof(float));
        std::memset(m_sSurrR.data(), 0, frames * sizeof(float));

        // Band 1 low-mid side through decorrelators [0, 1]
        {
            for (uint32_t i = 0; i < frames; ++i) {
                m_sAmbL[i] =  m_sSide1[i] * sw[1];
                m_sAmbR[i] = -m_sSide1[i] * sw[1];
            }
            m_spatialDecorr[0].Process(m_sAmbL.data(), m_sScratch.data(), frames);
            for (uint32_t i = 0; i < frames; ++i) {
                float tDuck = 1.0f - m_sTransients[i] * 0.5f;
                m_sSurrL[i] += m_sScratch[i] * 3.50f * ambFactor[1] * tDuck;
            }
            m_spatialDecorr[1].Process(m_sAmbR.data(), m_sScratch.data(), frames);
            for (uint32_t i = 0; i < frames; ++i) {
                float tDuck = 1.0f - m_sTransients[i] * 0.5f;
                m_sSurrR[i] += m_sScratch[i] * 3.50f * ambFactor[1] * tDuck;
            }
        }

        // Band 3 treble side (direct, no decorrelator — inverted for width)
        for (uint32_t i = 0; i < frames; ++i) {
            m_sSurrL[i] += m_sSide3[i] * 3.75f * ambFactor[3] * sw[3];
            m_sSurrR[i] -= m_sSide3[i] * 3.75f * ambFactor[3] * sw[3];
        }

        // Bands 0 + 2 fill (direct, modest gain + ambience boost)
        for (uint32_t i = 0; i < frames; ++i) {
            float fill = m_sSide0[i] * 1.50f * ambFactor[0] + m_sSide2[i] * 1.50f * ambFactor[2];
            m_sSurrL[i] += fill;
            m_sSurrR[i] -= fill;
        }

        // Top-back fold into surround (only when no physical top-back pair).
        // 50% of the would-be top-back blend (m_sSide2 * 5.0 + m_sSide3 * 7.0)
        // through decorrelators [6]/[7] keeps phase distinct from the
        // top-front fold (decorrelators [4]/[5]) so the renderer perceives
        // them as spatially separate cues rather than comb-filtering siblings.
        if (!hasTopBack) {
            // Full-bandwidth fold so the rear physical pair receives every
            // band of the would-be top-back content.
            for (uint32_t i = 0; i < frames; ++i) {
                m_sAmbL[i] = m_sSide0[i] * 1.50f + m_sSide1[i] * 1.50f
                           + m_sSide2[i] * 2.50f + m_sSide3[i] * 3.50f;
            }
            m_spatialDecorr[6].Process(m_sAmbL.data(), m_sScratch.data(), frames);
            for (uint32_t i = 0; i < frames; ++i) {
                float tDuck = 1.0f - m_sTransients[i] * 0.5f;
                m_sSurrL[i] += m_sScratch[i] * tDuck;
            }
            for (uint32_t i = 0; i < frames; ++i) {
                m_sAmbR[i] = -(m_sSide0[i] * 1.50f + m_sSide1[i] * 1.50f
                             + m_sSide2[i] * 2.50f + m_sSide3[i] * 3.50f);
            }
            m_spatialDecorr[7].Process(m_sAmbR.data(), m_sScratch.data(), frames);
            for (uint32_t i = 0; i < frames; ++i) {
                float tDuck = 1.0f - m_sTransients[i] * 0.5f;
                m_sSurrR[i] += m_sScratch[i] * tDuck;
            }
        }

        for (uint32_t i = 0; i < frames; ++i) {
            m_sSurrL[i] *= spatialExtGain;
            m_sSurrR[i] *= spatialExtGain;
        }
        // Feature 1+4: pre-delay + diffusion on sides
        {
            int len = m_rearDelayLength;
            for (uint32_t i = 0; i < frames; ++i) {
                float tmpL = m_rearDelayRing[0][m_rearDelaySidePos];
                float tmpR = m_rearDelayRing[1][m_rearDelaySidePos];
                m_rearDelayRing[0][m_rearDelaySidePos] = m_sSurrL[i];
                m_rearDelayRing[1][m_rearDelaySidePos] = m_sSurrR[i];
                m_sSurrL[i] = tmpL;
                m_sSurrR[i] = tmpR;
                m_rearDelaySidePos = (m_rearDelaySidePos + 1) % len;
            }
            m_rearDiffuser[0].Process(m_sSurrL.data(), frames);
            m_rearDiffuser[1].Process(m_sSurrR.data(), frames);
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
            m_sSurrL[i] = m_sScratch[i] * 5.00f * sw[2] * tDuck;
        }
        m_spatialDecorr[3].Process(m_sSide2.data(), m_sScratch.data(), frames);
        for (uint32_t i = 0; i < frames; ++i) {
            float tDuck = 1.0f - m_sTransients[i] * 0.5f;
            m_sSurrR[i] = m_sScratch[i] * 5.00f * sw[2] * tDuck;
        }

        // Bands 0 + 1 + 3 fill (direct, modest gain) — same full-bandwidth
        // principle as sides: every object carries every band so Dolby's
        // bass management has the freedom to crossover per the user's setup.
        // Inversion across L/R keeps the back-pair phase-distinct from sides.
        for (uint32_t i = 0; i < frames; ++i) {
            float fill = m_sSide0[i] * 1.50f + m_sSide1[i] * 1.25f + m_sSide3[i] * 1.25f;
            m_sSurrL[i] -= fill;
            m_sSurrR[i] += fill;
        }

        for (uint32_t i = 0; i < frames; ++i) {
            m_sSurrL[i] *= spatialExtGain;
            m_sSurrR[i] *= spatialExtGain;
        }
        // Feature 1+4: pre-delay + diffusion on backs
        {
            int len = m_rearDelayLength;
            for (uint32_t i = 0; i < frames; ++i) {
                float tmpL = m_rearDelayRing[2][m_rearDelayBackPos];
                float tmpR = m_rearDelayRing[3][m_rearDelayBackPos];
                m_rearDelayRing[2][m_rearDelayBackPos] = m_sSurrL[i];
                m_rearDelayRing[3][m_rearDelayBackPos] = m_sSurrR[i];
                m_sSurrL[i] = tmpL;
                m_sSurrR[i] = tmpR;
                m_rearDelayBackPos = (m_rearDelayBackPos + 1) % len;
            }
            m_rearDiffuser[2].Process(m_sSurrL.data(), frames);
            m_rearDiffuser[3].Process(m_sSurrR.data(), frames);
        }
        m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_BACK_LEFT, m_sSurrL.data(), frames);
        m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_BACK_RIGHT, m_sSurrR.data(), frames);
    }

    // ================================================================
    // HEIGHT SPLIT: side-signal driven (cancels mask-leak buzz exactly).
    // Top-front blend = bands 1+2 side; top-back blend = bands 2+3 side.
    //
    // Layout-aware folding: when the user's physical layout has no rear
    // height pair (anything other than 5.1.4 / 7.1.4), the top-back blend
    // is mixed INTO the top-front feed so all overhead bands (1+2+3) reach
    // the physical ceiling-front pair. Otherwise the renderer either drops
    // OBJ_TOP_BACK or folds it itself in an opaque way — better to handle
    // it deterministically here.
    // ================================================================
    {
        // hasTopBack was computed earlier (used by both surround-fold and
        // here). Layout_514 / Layout_714 are the only layouts with a real
        // physical top-back pair.

        // --- OBJ_TOP_FRONT_L/R: bands 1+2 residual SIDE (+ bands 2+3 when
        //     no physical top-back exists). Side cancels the per-bin
        //     mask-variation residue (same mask·M subtracted from both L
        //     and R, so (L_resid - R_resid)/2 erases it). Generous gain
        //     compensates for side being ~6–12 dB quieter than mid.
        for (uint32_t i = 0; i < frames; ++i) {
            // Full-bandwidth side blend. Band 0 (0–200 Hz) contributes the
            // 100–200 Hz warmth small ceiling drivers can actually play
            // (sub-100 Hz harmlessly rolls off in the driver itself). Band
            // 1 anchors the low-mid; band 2 the presence; band 3 included
            // gently so the spectrum is complete but the small drivers
            // don't over-emphasise the top. The (!hasTopBack) fold adds
            // extra band 2/3 to substitute for the missing physical
            // top-back pair.
            float frontBlend = m_sSide0[i] * 6.0f + m_sSide1[i] * 6.0f
                             + m_sSide2[i] * 4.5f + m_sSide3[i] * 1.5f;
            if (!hasTopBack) {
                frontBlend += m_sSide2[i] * 2.0f + m_sSide3[i] * 1.5f;
            }
            float tDuck = 1.0f - m_sTransients[i] * 0.5f;
            m_sAmbL[i] =  frontBlend * tDuck;
            m_sAmbR[i] = -frontBlend * tDuck;
        }
        m_spatialDecorr[4].Process(m_sAmbL.data(), m_sHeightL.data(), frames);
        m_spatialDecorr[5].Process(m_sAmbR.data(), m_sHeightR.data(), frames);
        for (uint32_t i = 0; i < frames; ++i) {
            m_sHeightL[i] *= spatialExtGain;
            m_sHeightR[i] *= spatialExtGain;
        }
        m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_TOP_FRONT_L, m_sHeightL.data(), frames);
        m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_TOP_FRONT_R, m_sHeightR.data(), frames);

        // --- OBJ_TOP_BACK_L/R: only fed when the layout has a physical
        //     rear-height pair. Otherwise submit silence (its content was
        //     already folded into top-front above).
        if (hasTopBack) {
            // Bands 0+1+2+3 — full-bandwidth so Dolby can bass-manage the
            // physical top-back drivers (which may have their own crossover
            // to LFE just like any other speaker pair).
            for (uint32_t i = 0; i < frames; ++i) {
                float backBlend = m_sSide0[i] * 2.0f + m_sSide1[i] * 2.0f
                                + m_sSide2[i] * 3.5f + m_sSide3[i] * 5.0f;
                float tDuck = 1.0f - m_sTransients[i] * 0.5f;
                m_sAmbL[i] =  backBlend * tDuck;
                m_sAmbR[i] = -backBlend * tDuck;
            }
            m_spatialDecorr[6].Process(m_sAmbL.data(), m_sHeightL.data(), frames);
            m_spatialDecorr[7].Process(m_sAmbR.data(), m_sHeightR.data(), frames);
            for (uint32_t i = 0; i < frames; ++i) {
                m_sHeightL[i] *= spatialExtGain;
                m_sHeightR[i] *= spatialExtGain;
            }
            m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_TOP_BACK_L, m_sHeightL.data(), frames);
            m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_TOP_BACK_R, m_sHeightR.data(), frames);
        } else {
            const float* silence = m_silenceBuffer.data();
            // Defensive: m_silenceBuffer should be sized in effSetBlockSize,
            // but if a host pushed an oversized block we use m_sScratch
            // (already zero-able) as fallback.
            if (m_silenceBuffer.size() >= frames) {
                m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_TOP_BACK_L, silence, frames);
                m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_TOP_BACK_R, silence, frames);
            } else {
                std::memset(m_sScratch.data(), 0, frames * sizeof(float));
                m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_TOP_BACK_L, m_sScratch.data(), frames);
                m_spatialWriter.SubmitObjectAudio(SpatialObjectWriter::OBJ_TOP_BACK_R, m_sScratch.data(), frames);
            }
        }
    }

    // Write original L/R to channel outputs as fallback. The caller
    // (ProcessReplacing) will zero these if the spatial writer is active.
    std::memcpy(outputs[CH_FL], L, sampleFrames * sizeof(float));
    std::memcpy(outputs[CH_FR], R, sampleFrames * sizeof(float));
    for (int ch = CH_C; ch < kAtmosChannelCount; ++ch) {
        std::memset(outputs[ch], 0, sampleFrames * sizeof(float));
    }
}

// ============================================================================
// Multichannel-to-Object promotion path — zero DSP, zero latency.
//
// For 5.1, 7.1, and 7.1.4 (Passthrough) input, each channel is promoted 1:1
// to a positioned Atmos object at its ITU reference position. The Dolby
// renderer then handles mapping onto whatever physical speakers exist.
//
// No spectral separation, no delay, no remixing. Genuine multichannel content
// (games, films) keeps its directional cues intact. Any stereo content that
// Windows mixed into the front pair stays in OBJ_LEFT/OBJ_RIGHT unaltered —
// the tradeoff: stereo apps lose spatial enhancement when a multichannel
// source is active, but multichannel fidelity is preserved.
// ============================================================================
void MagicSpatialVst::ProcessMultichannelObjects(float** inputs, float** outputs,
                                                  VstInt32 sampleFrames, InputLayout layout) {
    uint32_t frames = static_cast<uint32_t>(sampleFrames);

    // Lazy-grow the silence buffer in case effSetBlockSize was never called
    // (host bug) or this block is unexpectedly larger than the negotiated size.
    if (m_silenceBuffer.size() < frames) {
        m_silenceBuffer.assign(frames, 0.0f);
    }
    const float* silence = m_silenceBuffer.data();

    // Wipe dynamic positions left over from the stereo path, then re-apply
    // the user-configured surround override (Side vs Rear).
    m_spatialWriter.ResetObjectPositionsToReference();
    ApplySurroundPositionOverride();

    // --- Submit each input channel directly to its Atmos object slot ---
    bool fed[SpatialObjectWriter::OBJ_COUNT] = { false };
    auto submit = [&](SpatialObjectWriter::ObjectSlot slot, const float* data) {
        m_spatialWriter.SubmitObjectAudio(slot, data, frames);
        fed[slot] = true;
    };

    int inputChannelCount = 0;
    switch (layout) {
        case InputLayout::Surround51:
            // 6 channels: [0]FL [1]FR [2]C [3]LFE [4]SL [5]SR
            inputChannelCount = 6;
            submit(SpatialObjectWriter::OBJ_LEFT,       inputs[0]);
            submit(SpatialObjectWriter::OBJ_RIGHT,      inputs[1]);
            submit(SpatialObjectWriter::OBJ_VOCAL,      inputs[2]);
            submit(SpatialObjectWriter::OBJ_SUBBASS,    inputs[3]);
            submit(SpatialObjectWriter::OBJ_SIDE_LEFT,  inputs[4]);
            submit(SpatialObjectWriter::OBJ_SIDE_RIGHT, inputs[5]);
            break;

        case InputLayout::Surround71:
            // 8 channels: [0]FL [1]FR [2]C [3]LFE [4]BL [5]BR [6]SL [7]SR
            inputChannelCount = 8;
            submit(SpatialObjectWriter::OBJ_LEFT,       inputs[0]);
            submit(SpatialObjectWriter::OBJ_RIGHT,      inputs[1]);
            submit(SpatialObjectWriter::OBJ_VOCAL,      inputs[2]);
            submit(SpatialObjectWriter::OBJ_SUBBASS,    inputs[3]);
            submit(SpatialObjectWriter::OBJ_BACK_LEFT,  inputs[4]);
            submit(SpatialObjectWriter::OBJ_BACK_RIGHT, inputs[5]);
            submit(SpatialObjectWriter::OBJ_SIDE_LEFT,  inputs[6]);
            submit(SpatialObjectWriter::OBJ_SIDE_RIGHT, inputs[7]);
            break;

        case InputLayout::Passthrough:
            // 12 channels in AtmosChannel order:
            // [0]FL [1]FR [2]C [3]LFE [4]SL [5]SR [6]BL [7]BR [8]TFL [9]TFR [10]TBL [11]TBR
            inputChannelCount = 12;
            submit(SpatialObjectWriter::OBJ_LEFT,        inputs[0]);
            submit(SpatialObjectWriter::OBJ_RIGHT,       inputs[1]);
            submit(SpatialObjectWriter::OBJ_VOCAL,       inputs[2]);
            submit(SpatialObjectWriter::OBJ_SUBBASS,     inputs[3]);
            submit(SpatialObjectWriter::OBJ_SIDE_LEFT,   inputs[4]);
            submit(SpatialObjectWriter::OBJ_SIDE_RIGHT,  inputs[5]);
            submit(SpatialObjectWriter::OBJ_BACK_LEFT,   inputs[6]);
            submit(SpatialObjectWriter::OBJ_BACK_RIGHT,  inputs[7]);
            submit(SpatialObjectWriter::OBJ_TOP_FRONT_L, inputs[8]);
            submit(SpatialObjectWriter::OBJ_TOP_FRONT_R, inputs[9]);
            submit(SpatialObjectWriter::OBJ_TOP_BACK_L,  inputs[10]);
            submit(SpatialObjectWriter::OBJ_TOP_BACK_R,  inputs[11]);
            break;

        default:
            return;
    }

    // Silence for unfed slots.
    for (int i = 0; i < SpatialObjectWriter::OBJ_COUNT; ++i) {
        if (!fed[i]) {
            m_spatialWriter.SubmitObjectAudio(static_cast<SpatialObjectWriter::ObjectSlot>(i),
                                              silence, frames);
        }
    }

    // 1:1 channel-passthrough fallback (raw inputs — no delay since no DSP).
    for (int ch = 0; ch < inputChannelCount && ch < kAtmosChannelCount; ++ch) {
        std::memcpy(outputs[ch], inputs[ch], sampleFrames * sizeof(float));
    }
    for (int ch = inputChannelCount; ch < kAtmosChannelCount; ++ch) {
        std::memset(outputs[ch], 0, sampleFrames * sizeof(float));
    }
}

} // namespace MagicSpatial
