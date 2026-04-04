#include "vst/MagicSpatialVst.h"

#include <windows.h>
#include <commctrl.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

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
            if (id == ID_MODE_COMBO) {
                // Map combo index to param: 0=Auto(0.0), 1=Stereo(0.25), 2=5.1(0.5), 3=7.1(0.75), 4=Pass(1.0)
                float val = static_cast<float>(sel) * 0.25f;
                plugin->SetParameter(0, val);
            }
            else if (id == ID_SPEAKERS_COMBO) {
                // Map: 0=2.0(0.0), 1=5.1.2(0.25), 2=5.1.4(0.45), 3=7.1.2(0.65), 4=7.1.4(1.0)
                float val = (sel == 0) ? 0.0f : (sel == 1) ? 0.25f : (sel == 2) ? 0.45f : (sel == 3) ? 0.65f : 1.0f;
                plugin->SetParameter(1, val);
            }
        }
        return 0;

    case WM_HSCROLL: {
        if (!plugin) break;
        HWND slider = reinterpret_cast<HWND>(lParam);
        int pos = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        float val = static_cast<float>(pos) / 100.0f;
        int id = GetDlgCtrlID(slider);

        if (id == ID_HEIGHT_SLIDER) {
            plugin->SetParameter(2, val);
            // Update label
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d%%", pos);
            SetDlgItemTextA(hwnd, ID_HEIGHT_LABEL, buf);
        }
        else if (id == ID_SURROUND_SLIDER) {
            plugin->SetParameter(3, val);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d%%", pos);
            SetDlgItemTextA(hwnd, ID_SURROUND_LABEL, buf);
        }
        return 0;
    }

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
        return 0;

    case effClose:
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
        y += 30;

        // --- Height Gain slider ---
        CreateWindowW(L"STATIC", L"Height:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, y + 2, labelW, 20, hwnd, nullptr, hInst, nullptr);
        HWND hSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            ctrlX, y, ctrlW - 45, 25, hwnd,
            reinterpret_cast<HMENU>(ID_HEIGHT_SLIDER), hInst, nullptr);
        SendMessageW(hSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(hSlider, TBM_SETPOS, TRUE, static_cast<int>(m_paramHeightGain * 100.0f));
        // Value label
        char hBuf[16];
        std::snprintf(hBuf, sizeof(hBuf), "%d%%", static_cast<int>(m_paramHeightGain * 100.0f));
        CreateWindowA("STATIC", hBuf,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            ctrlX + ctrlW - 40, y + 4, 40, 20, hwnd,
            reinterpret_cast<HMENU>(ID_HEIGHT_LABEL), hInst, nullptr);
        y += 30;

        // --- Surround Gain slider ---
        CreateWindowW(L"STATIC", L"Surround:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, y + 2, labelW, 20, hwnd, nullptr, hInst, nullptr);
        HWND sSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            ctrlX, y, ctrlW - 45, 25, hwnd,
            reinterpret_cast<HMENU>(ID_SURROUND_SLIDER), hInst, nullptr);
        SendMessageW(sSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(sSlider, TBM_SETPOS, TRUE, static_cast<int>(m_paramSurrGain * 100.0f));
        char sBuf[16];
        std::snprintf(sBuf, sizeof(sBuf), "%d%%", static_cast<int>(m_paramSurrGain * 100.0f));
        CreateWindowA("STATIC", sBuf,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            ctrlX + ctrlW - 40, y + 4, 40, 20, hwnd,
            reinterpret_cast<HMENU>(ID_SURROUND_LABEL), hInst, nullptr);

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
            case 2: std::strncpy(buf, "Height",   kVstMaxParamStrLen); break;
            case 3: std::strncpy(buf, "Surround", kVstMaxParamStrLen); break;
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
            case 2:
                std::snprintf(buf, kVstMaxParamStrLen, "%.0f%%", m_paramHeightGain * 100.0f);
                break;
            case 3:
                std::snprintf(buf, kVstMaxParamStrLen, "%.0f%%", m_paramSurrGain * 100.0f);
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
        m_paramHeightGain = value;
        break;
    case 3:
        m_paramSurrGain = value;
        break;
    }
}

float MagicSpatialVst::GetParameter(VstInt32 index) {
    switch (index) {
    case 0: return m_paramMode;
    case 1: return m_paramSpeakers;
    case 2: return m_paramHeightGain;
    case 3: return m_paramSurrGain;
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

    // Scale synthesized heights by Height Gain parameter
    if (m_currentLayout != InputLayout::Passthrough) {
        const float hGain = m_paramHeightGain;
        for (int ch = CH_TFL; ch <= CH_TBR; ++ch) {
            float* buf = outputs[ch];
            for (VstInt32 i = 0; i < sampleFrames; ++i) {
                buf[i] *= hGain;
            }
        }
    }

    // Zero channels with no physical speakers
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
        return energy > 1e-10f;
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

} // namespace MagicSpatial
