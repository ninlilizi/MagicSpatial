#pragma once

// Minimal VST2 type definitions.
// These are the essential structures and constants needed to implement a VST2 plugin.
// Based on the public VST2 specification by Steinberg.

#include <cstdint>

// VST calling convention
#if defined(_WIN32)
#define VSTCALLBACK __cdecl
#else
#define VSTCALLBACK
#endif

using VstInt32 = int32_t;
using VstIntPtr = intptr_t;

// The magic number identifying a valid AEffect
constexpr VstInt32 kEffectMagic = 0x56737450; // 'VstP'

// Forward declaration
struct AEffect;

// Host callback function type
using audioMasterCallback = VstIntPtr(VSTCALLBACK*)(AEffect* effect, VstInt32 opcode,
    VstInt32 index, VstIntPtr value, void* ptr, float opt);

// The main VST2 effect structure
struct AEffect {
    VstInt32 magic;

    VstIntPtr(VSTCALLBACK* dispatcher)(AEffect*, VstInt32 opcode, VstInt32 index,
        VstIntPtr value, void* ptr, float opt);

    // Deprecated process callback (set to nullptr)
    void(VSTCALLBACK* DEPRECATED_process)(AEffect*, float**, float**, VstInt32);

    void(VSTCALLBACK* setParameter)(AEffect*, VstInt32 index, float value);
    float(VSTCALLBACK* getParameter)(AEffect*, VstInt32 index);

    VstInt32 numPrograms;
    VstInt32 numParams;
    VstInt32 numInputs;
    VstInt32 numOutputs;
    VstInt32 flags;

    VstIntPtr resvd1;
    VstIntPtr resvd2;

    VstInt32 initialDelay;

    VstInt32 DEPRECATED_realQualities;
    VstInt32 DEPRECATED_offQualities;
    float    DEPRECATED_ioRatio;

    void* object;    // Pointer to plugin class instance
    void* user;

    VstInt32 uniqueID;
    VstInt32 version;

    void(VSTCALLBACK* processReplacing)(AEffect*, float** inputs, float** outputs, VstInt32 sampleFrames);
    void(VSTCALLBACK* processDoubleReplacing)(AEffect*, double** inputs, double** outputs, VstInt32 sampleFrames);

    char future[56];
};

// Editor rectangle
struct ERect {
    int16_t top;
    int16_t left;
    int16_t bottom;
    int16_t right;
};

// AEffect flags
enum VstAEffectFlags {
    effFlagsHasEditor     = 1 << 0,
    effFlagsCanReplacing  = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth       = 1 << 8,
    effFlagsNoSoundInStop = 1 << 9,
    effFlagsCanDoubleReplacing = 1 << 12,
};

// Dispatcher opcodes
enum VstOpcodes {
    effOpen = 0,
    effClose = 1,
    effSetProgram = 2,
    effGetProgram = 3,
    effGetProgramName = 5,
    effGetParamLabel = 6,
    effGetParamDisplay = 7,
    effGetParamName = 8,
    effSetSampleRate = 10,
    effSetBlockSize = 11,
    effMainsChanged = 12,
    effEditGetRect = 13,
    effEditOpen = 14,
    effEditClose = 15,
    effEditIdle = 19,
    effGetChunk = 23,
    effSetChunk = 24,
    effCanBeAutomated = 26,
    effGetEffectName = 45,
    effGetVendorString = 47,
    effGetProductString = 48,
    effGetVendorVersion = 49,
    effCanDo = 51,
    effGetTailSize = 52,
    effGetNumMidiInputChannels = 74,
    effGetNumMidiOutputChannels = 75,
};

// audioMaster opcodes
enum VstAudioMasterOpcodes {
    audioMasterAutomate = 0,
    audioMasterVersion = 1,
    audioMasterCurrentId = 2,
    audioMasterIdle = 3,
};

// String length constants
constexpr int kVstMaxEffectNameLen = 64;
constexpr int kVstMaxParamStrLen   = 8;
constexpr int kVstMaxVendorStrLen  = 64;
constexpr int kVstMaxProductStrLen = 64;
