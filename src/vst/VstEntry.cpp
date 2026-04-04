#include "vst/MagicSpatialVst.h"

// VST2 plugin entry point.
// The host calls this function to create a new plugin instance.
extern "C" {

#if defined(_WIN32)
__declspec(dllexport)
#endif
AEffect* VSTPluginMain(audioMasterCallback audioMaster) {
    // Check that the host supports VST2
    if (!audioMaster) return nullptr;

    VstIntPtr hostVersion = audioMaster(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f);
    if (hostVersion < 2) return nullptr;  // Need at least VST 2.0

    auto* plugin = new MagicSpatial::MagicSpatialVst(audioMaster);
    return plugin->GetAEffect();
}

} // extern "C"

// DLL entry point (Windows)
#if defined(_WIN32)
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
#endif
