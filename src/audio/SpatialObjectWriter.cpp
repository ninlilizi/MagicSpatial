#include "audio/SpatialObjectWriter.h"
#include "core/Log.h"

#include <objbase.h>
#include <propsys.h>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace MagicSpatial {

#define Log LogMsg

// Default 3D positions for each object slot.
// Angles follow ITU-R BS.2051 reference geometry for 7.1.4 / 5.1.4 rooms.
// Coordinate system: +X right, +Y up, -Z forward. All positions lie on the
// unit sphere (|pos| ~= 1) except SUBBASS which is placed slightly grounded.
const SpatialObjectWriter::ObjectPosition SpatialObjectWriter::kDefaultPositions[OBJ_COUNT] = {
    { 0.000f, -0.300f,  0.000f}, // OBJ_SUBBASS:     centre, grounded (low-freq omnidirectional)
    { 0.000f,  0.000f, -0.700f}, // OBJ_VOCAL:        0° azimuth, 0° elevation  (intimate, pitch-tracked)
    {-0.500f,  0.000f, -0.866f}, // OBJ_LEFT:       -30° azimuth, 0° elevation
    { 0.500f,  0.000f, -0.866f}, // OBJ_RIGHT:      +30° azimuth, 0° elevation
    {-0.985f,  0.000f,  0.174f}, // OBJ_SURR_LEFT: -100° azimuth, 0° elevation  (side, slightly rear)
    { 0.985f,  0.000f,  0.174f}, // OBJ_SURR_RIGHT:+100° azimuth, 0° elevation
    {-0.354f,  0.707f, -0.612f}, // OBJ_HEIGHT_LEFT: -30° azimuth, +45° elevation (top front L)
    { 0.354f,  0.707f, -0.612f}, // OBJ_HEIGHT_RIGHT:+30° azimuth, +45° elevation (top front R)
};

// --- IActivateAudioInterfaceCompletionHandler for async activation ---

class SpatialActivationHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    SpatialActivationHandler() {
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~SpatialActivationHandler() {
        if (m_event) CloseHandle(m_event);
    }

    HRESULT WaitForResult(ComPtr<ISpatialAudioClient>& outClient) {
        WaitForSingleObject(m_event, 10000); // 10 second timeout
        if (SUCCEEDED(m_hr)) outClient = m_client;
        return m_hr;
    }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return m_ref.fetch_add(1) + 1; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG c = m_ref.fetch_sub(1) - 1;
        if (c == 0) delete this;
        return c;
    }

    // IActivateAudioInterfaceCompletionHandler
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override {
        IUnknown* unk = nullptr;
        HRESULT hrAct = E_FAIL;
        HRESULT hr = op->GetActivateResult(&hrAct, &unk);
        if (SUCCEEDED(hr) && SUCCEEDED(hrAct) && unk) {
            unk->QueryInterface(IID_PPV_ARGS(&m_client));
            m_hr = S_OK;
            unk->Release();
        } else {
            m_hr = FAILED(hrAct) ? hrAct : hr;
        }
        SetEvent(m_event);
        return S_OK;
    }

private:
    std::atomic<ULONG> m_ref{1};
    HANDLE m_event = nullptr;
    HRESULT m_hr = E_FAIL;
    ComPtr<ISpatialAudioClient> m_client;
};

// --- ISpatialAudioObjectRenderStreamNotify (minimal) ---

class SpatialNotify : public ISpatialAudioObjectRenderStreamNotify {
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISpatialAudioObjectRenderStreamNotify)) {
            *ppv = static_cast<ISpatialAudioObjectRenderStreamNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return m_ref.fetch_add(1) + 1; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG c = m_ref.fetch_sub(1) - 1;
        if (c == 0) delete this;
        return c;
    }
    STDMETHOD(OnAvailableDynamicObjectCountChange)(
        ISpatialAudioObjectRenderStreamBase*, LONGLONG, UINT32) override {
        return S_OK;
    }
private:
    std::atomic<ULONG> m_ref{1};
};

// --- SpatialObjectWriter implementation ---

SpatialObjectWriter::SpatialObjectWriter() {
    m_renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (int i = 0; i < OBJ_COUNT; ++i) {
        m_objects[i].position = kDefaultPositions[i];
    }
}

SpatialObjectWriter::~SpatialObjectWriter() {
    Shutdown();
    if (m_renderEvent) {
        CloseHandle(m_renderEvent);
        m_renderEvent = nullptr;
    }
}

bool SpatialObjectWriter::Initialize() {
    Log("Initialize() called\n");
    if (m_active || m_thread.joinable()) {
        Log("Initialize() skipped — already active or thread running\n");
        return false;
    }

    m_shutdownRequested = false;
    m_failed = false;
    m_active = false;

    // Allocate exchange buffers
    for (auto& buf : m_buffers) {
        buf.data.resize(4096, 0.0f);  // will grow as needed
        buf.validFrames = 0;
        buf.hasNewData = false;
    }

    m_thread = std::thread(&SpatialObjectWriter::BackgroundThreadFunc, this);
    return true;
}

void SpatialObjectWriter::SetObjectPosition(ObjectSlot slot, float x, float y, float z) {
    if (slot < 0 || slot >= OBJ_COUNT) return;
    // Thread-safe: position is read by the render thread per cycle.
    // Simple struct write — no lock needed for float members on x64.
    m_objects[slot].position = {x, y, z};
}

void SpatialObjectWriter::Shutdown() {
    Log("[SpatialObjectWriter] Shutdown requested\n");
    m_shutdownRequested = true;
    if (m_renderEvent) SetEvent(m_renderEvent);

    if (m_thread.joinable()) {
        m_thread.join();
    }

    // Release in order: objects → stream → client
    for (auto& obj : m_objects) {
        obj.object.Reset();
        obj.active = false;
    }

    if (m_renderStream) {
        m_renderStream->Stop();
        m_renderStream->Reset();
        m_renderStream.Reset();
    }

    m_spatialClient.Reset();
    m_active = false;

    // Brief pause to let the audio subsystem fully release the stream
    Sleep(100);
    Log("[SpatialObjectWriter] Shutdown complete\n");
}

void SpatialObjectWriter::SubmitObjectAudio(ObjectSlot slot, const float* data, uint32_t frameCount) {
    if (slot < 0 || slot >= OBJ_COUNT || !data || frameCount == 0) return;

    std::lock_guard<std::mutex> lock(m_bufferMutex);
    auto& buf = m_buffers[slot];
    if (buf.data.size() < frameCount) {
        buf.data.resize(frameCount);
    }
    std::memcpy(buf.data.data(), data, frameCount * sizeof(float));
    buf.validFrames = frameCount;
    buf.hasNewData.store(true, std::memory_order_release);
}

void SpatialObjectWriter::BackgroundThreadFunc() {
    // Initialize COM as MTA on this thread
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Log("[SpatialObjectWriter] COM init failed: 0x%08lX\n",
            static_cast<unsigned long>(hr));
        m_failed = true;
        return;
    }

    Log("[SpatialObjectWriter] Background thread started, activating spatial audio...\n");

    if (!ActivateSpatialAudio()) {
        Log("[SpatialObjectWriter] Spatial audio activation FAILED\n");
        m_failed = true;
        CoUninitialize();
        return;
    }

    Log("[SpatialObjectWriter] Spatial audio ACTIVE! Entering render loop.\n");
    m_active = true;

    RenderLoop();

    CoUninitialize();
}

bool SpatialObjectWriter::ActivateSpatialAudio() {
    // Get default render device
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        Log("[SpatialObjectWriter] Failed to create MMDeviceEnumerator: 0x%08lX\n",
            static_cast<unsigned long>(hr));
        return false;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (FAILED(hr)) {
        Log("[SpatialObjectWriter] Failed to get default render device: 0x%08lX\n",
            static_cast<unsigned long>(hr));
        return false;
    }

    // Direct activation via IMMDevice::Activate (works for Home Theater mode)
    hr = device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(m_spatialClient.GetAddressOf()));

    if (FAILED(hr) || !m_spatialClient) {
        Log("[SpatialObjectWriter] IMMDevice::Activate for ISpatialAudioClient failed: 0x%08lX\n",
            static_cast<unsigned long>(hr));
        return false;
    }

    Log("[SpatialObjectWriter] ISpatialAudioClient obtained via IMMDevice::Activate!\n");

    // Get supported format
    ComPtr<IAudioFormatEnumerator> formatEnum;
    hr = m_spatialClient->GetSupportedAudioObjectFormatEnumerator(&formatEnum);
    if (FAILED(hr)) return false;

    hr = formatEnum->GetFormat(0, &m_objectFormat);
    if (FAILED(hr)) return false;

    Log("[SpatialObjectWriter] Object format: %u Hz, %u-bit\n",
        m_objectFormat->nSamplesPerSec, m_objectFormat->wBitsPerSample);

    // Get max dynamic object count
    UINT32 maxDynamic = 0;
    hr = m_spatialClient->GetMaxDynamicObjectCount(&maxDynamic);
    Log("[SpatialObjectWriter] Max dynamic objects: %u\n", maxDynamic);

    if (maxDynamic < OBJ_COUNT) {
        Log("[SpatialObjectWriter] WARNING: need %d objects but max is %u\n",
            OBJ_COUNT, maxDynamic);
        // Continue anyway — we'll activate as many as we can
    }

    // Create render stream with dynamic objects
    auto* notify = new SpatialNotify();

    SpatialAudioObjectRenderStreamActivationParams streamParams{};
    streamParams.ObjectFormat = m_objectFormat;
    streamParams.StaticObjectTypeMask = AudioObjectType_None; // No static objects
    streamParams.MinDynamicObjectCount = 1;
    streamParams.MaxDynamicObjectCount = static_cast<UINT32>(OBJ_COUNT);
    streamParams.Category = AudioCategory_Media;
    streamParams.EventHandle = m_renderEvent;
    streamParams.NotifyObject = notify;

    PROPVARIANT activateParams;
    PropVariantInit(&activateParams);
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(streamParams);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&streamParams);

    // Retry stream activation — previous instance may not have fully released yet
    for (int attempt = 0; attempt < 100; ++attempt) {
        hr = m_spatialClient->ActivateSpatialAudioStream(
            &activateParams, __uuidof(ISpatialAudioObjectRenderStream),
            reinterpret_cast<void**>(m_renderStream.GetAddressOf()));

        if (SUCCEEDED(hr)) break;

        if (hr == 0x88890103 /*SPTLAUDCLNT_E_STREAM_NOT_AVAILABLE*/ && attempt < 99) {
            if (attempt % 10 == 0) {
                Log("[SpatialObjectWriter] Stream not available yet, retry %d/100...\n", attempt + 1);
            }
            Sleep(200);
        } else {
            Log("[SpatialObjectWriter] ActivateSpatialAudioStream failed: 0x%08lX\n",
                static_cast<unsigned long>(hr));
            return false;
        }
    }

    // Get max frame count
    hr = m_spatialClient->GetMaxFrameCount(m_objectFormat, &m_maxFrameCount);
    if (FAILED(hr)) return false;

    Log("[SpatialObjectWriter] Max frames per cycle: %u\n", m_maxFrameCount);

    // Activate dynamic objects
    for (int i = 0; i < OBJ_COUNT && i < static_cast<int>(maxDynamic); ++i) {
        hr = m_renderStream->ActivateSpatialAudioObject(
            AudioObjectType_Dynamic, &m_objects[i].object);
        if (SUCCEEDED(hr)) {
            m_objects[i].active = true;
            Log("[SpatialObjectWriter] Object %d activated\n", i);
        } else {
            Log("[SpatialObjectWriter] Object %d failed: 0x%08lX\n",
                i, static_cast<unsigned long>(hr));
        }
    }

    // Start the stream
    hr = m_renderStream->Start();
    if (FAILED(hr)) {
        Log("[SpatialObjectWriter] Start failed: 0x%08lX\n",
            static_cast<unsigned long>(hr));
        return false;
    }

    return true;
}

void SpatialObjectWriter::RenderLoop() {
    std::vector<float> silenceBuffer(m_maxFrameCount, 0.0f);

    while (!m_shutdownRequested) {
        DWORD waitResult = WaitForSingleObject(m_renderEvent, 500);
        if (m_shutdownRequested) break;
        if (waitResult == WAIT_TIMEOUT) continue;

        UINT32 availableDynamic = 0;
        UINT32 frameCount = 0;
        HRESULT hr = m_renderStream->BeginUpdatingAudioObjects(&availableDynamic, &frameCount);
        if (FAILED(hr)) {
            if (hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED) {
                Log("[SpatialObjectWriter] Resources invalidated\n");
                break;
            }
            continue;
        }

        // Write audio to each active object
        for (int i = 0; i < OBJ_COUNT; ++i) {
            if (!m_objects[i].active || !m_objects[i].object) continue;

            // Set position
            auto& pos = m_objects[i].position;
            m_objects[i].object->SetPosition(pos.x, pos.y, pos.z);
            m_objects[i].object->SetVolume(1.0f);

            // Get buffer and fill it
            BYTE* buffer = nullptr;
            UINT32 bufferLength = 0;
            hr = m_objects[i].object->GetBuffer(&buffer, &bufferLength);
            if (FAILED(hr) || !buffer) continue;

            UINT32 bytesToWrite = frameCount * sizeof(float);

            // Check if audio thread has submitted data for this object
            bool hasData = false;
            {
                std::lock_guard<std::mutex> lock(m_bufferMutex);
                auto& objBuf = m_buffers[i];
                if (objBuf.hasNewData.load(std::memory_order_acquire)) {
                    UINT32 copyFrames = (frameCount < objBuf.validFrames) ? frameCount : objBuf.validFrames;
                    std::memcpy(buffer, objBuf.data.data(), copyFrames * sizeof(float));
                    // Zero-fill remainder if our buffer is shorter
                    if (copyFrames < frameCount) {
                        std::memset(buffer + copyFrames * sizeof(float), 0,
                            (frameCount - copyFrames) * sizeof(float));
                    }
                    objBuf.hasNewData.store(false, std::memory_order_release);
                    hasData = true;
                }
            }

            if (!hasData) {
                // No new data — write silence
                std::memset(buffer, 0, bytesToWrite);
            }
        }

        hr = m_renderStream->EndUpdatingAudioObjects();
        if (FAILED(hr)) {
            Log("[SpatialObjectWriter] EndUpdatingAudioObjects failed: 0x%08lX\n",
                static_cast<unsigned long>(hr));
        }
    }
}

} // namespace MagicSpatial
