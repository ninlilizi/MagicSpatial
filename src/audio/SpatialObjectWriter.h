#pragma once

#include "core/Types.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace MagicSpatial {

// Writes audio as positioned 3D objects via ISpatialAudioClient.
// Designed to run from within an E-APO VST DLL — activates spatial audio
// on the default render endpoint from a background MTA thread.
//
// Usage:
//   1. Call Initialize() once (spawns background thread, activates spatial audio)
//   2. Call SubmitFrame() from the audio thread to push decomposed audio
//   3. The background thread writes objects to Dolby Access
//   4. Call Shutdown() to clean up
class SpatialObjectWriter {
public:
    // Object slots for our spatial components (7.1.4 layout, 12 objects)
    enum ObjectSlot {
        OBJ_SUBBASS = 0,     // Centre, grounded
        OBJ_VOCAL,            // Front centre
        OBJ_LEFT,             // Front left
        OBJ_RIGHT,            // Front right
        OBJ_SIDE_LEFT,        // Side left (±90°)
        OBJ_SIDE_RIGHT,       // Side right
        OBJ_BACK_LEFT,        // Back left (±135°)
        OBJ_BACK_RIGHT,       // Back right
        OBJ_TOP_FRONT_L,      // Top front left (±30°, +45° elev)
        OBJ_TOP_FRONT_R,      // Top front right
        OBJ_TOP_BACK_L,       // Top back left (±150°, +45° elev)
        OBJ_TOP_BACK_R,       // Top back right
        OBJ_COUNT
    };

    struct ObjectPosition {
        float x, y, z;  // -1..+1 for each axis. y = up
    };

    SpatialObjectWriter();
    ~SpatialObjectWriter();

    // Attempt to activate ISpatialAudioClient. Returns true if successful.
    // Non-blocking: spawns a background thread that does the activation.
    bool Initialize();

    // Check if spatial audio is ready to receive objects.
    bool IsActive() const { return m_active.load(std::memory_order_acquire); }

    // Did activation fail? (call after a reasonable timeout)
    bool HasFailed() const { return m_failed.load(std::memory_order_acquire); }

    // Push one block of audio for a specific object slot.
    // Data is copied into an internal buffer. Thread-safe (audio thread calls this).
    void SubmitObjectAudio(ObjectSlot slot, const float* data, uint32_t frameCount);

    // Update the 3D position of an object. Takes effect on the next render cycle.
    void SetObjectPosition(ObjectSlot slot, float x, float y, float z);

    void Shutdown();

private:
    void BackgroundThreadFunc();
    bool ActivateSpatialAudio();
    void RenderLoop();

    // Spatial audio COM objects
    ComPtr<ISpatialAudioClient> m_spatialClient;
    ComPtr<ISpatialAudioObjectRenderStream> m_renderStream;

    struct DynamicObject {
        ComPtr<ISpatialAudioObject> object;
        ObjectPosition position;
        bool active = false;
    };
    DynamicObject m_objects[OBJ_COUNT];

    HANDLE m_renderEvent = nullptr;
    UINT32 m_maxFrameCount = 0;
    WAVEFORMATEX* m_objectFormat = nullptr;

    // Background thread
    std::thread m_thread;
    std::atomic<bool> m_shutdownRequested{false};
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_failed{false};

    // Audio exchange buffers (per object)
    // Audio thread writes, render thread reads. Protected by a simple spin mutex.
    struct ObjectBuffer {
        std::vector<float> data;
        uint32_t validFrames = 0;
        std::atomic<bool> hasNewData{false};
    };
    ObjectBuffer m_buffers[OBJ_COUNT];
    std::mutex m_bufferMutex;

    // Default object positions
    static const ObjectPosition kDefaultPositions[OBJ_COUNT];
};

} // namespace MagicSpatial
