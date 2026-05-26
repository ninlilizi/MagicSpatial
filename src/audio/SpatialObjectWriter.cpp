#include "audio/SpatialObjectWriter.h"
#include "core/Log.h"

#include <objbase.h>
#include <propsys.h>
#include <avrt.h>
#include <cstdio>
#include <cstring>
#include <cmath>

#pragma comment(lib, "avrt.lib")

namespace MagicSpatial {

#define Log LogMsg

namespace {
    // Yield/reclaim debouncing. We step aside promptly when a peer spatial
    // client starves us, but return cautiously — the pool must stay roomy for
    // a sustained spell before we re-claim, so a fluttering neighbour can't
    // ping-pong us in and out (the oscillation hazard of an eager reclaim).
    constexpr ULONGLONG kContentionDebounceMs = 750;
    constexpr ULONGLONG kReclaimDebounceMs    = 3000;

    // Render-event wait. The spatial engine normally signals every quantum
    // (~10 ms). If the source stalls (e.g. a network re-buffer), the event
    // falls silent — we wake on this timeout instead and push a fresh frame of
    // silence so the engine has no stale buffer to repeat as a buzz.
    constexpr DWORD kRenderWaitMs = 64;
}

// Default 3D positions for each object slot.
// Angles follow ITU-R BS.2051 reference geometry for 7.1.4 rooms.
// Coordinate system: +X right, +Y up, -Z forward. All positions lie on the
// unit sphere (|pos| ~= 1) except SUBBASS which is placed slightly grounded.
const SpatialObjectWriter::ObjectPosition SpatialObjectWriter::kDefaultPositions[OBJ_COUNT] = {
    { 0.000f, -0.300f,  0.000f}, // OBJ_SUBBASS:     centre, grounded
    { 0.000f,  0.000f, -1.000f}, // OBJ_VOCAL:        0° azimuth, 0° elevation  (reference)
    {-0.500f,  0.000f, -0.866f}, // OBJ_LEFT:       -30° azimuth, 0° elevation
    { 0.500f,  0.000f, -0.866f}, // OBJ_RIGHT:      +30° azimuth, 0° elevation
    {-1.000f,  0.000f,  0.000f}, // OBJ_SIDE_LEFT:  -90° azimuth, 0° elevation
    { 1.000f,  0.000f,  0.000f}, // OBJ_SIDE_RIGHT: +90° azimuth, 0° elevation
    {-0.707f,  0.000f,  0.707f}, // OBJ_BACK_LEFT: -135° azimuth, 0° elevation
    { 0.707f,  0.000f,  0.707f}, // OBJ_BACK_RIGHT:+135° azimuth, 0° elevation
    {-0.354f,  0.707f, -0.612f}, // OBJ_TOP_FRONT_L: -30° azimuth, +45° elevation
    { 0.354f,  0.707f, -0.612f}, // OBJ_TOP_FRONT_R: +30° azimuth, +45° elevation
    {-0.354f,  0.707f,  0.612f}, // OBJ_TOP_BACK_L: -150° azimuth, +45° elevation
    { 0.354f,  0.707f,  0.612f}, // OBJ_TOP_BACK_R: +150° azimuth, +45° elevation
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
    // Optional sink the writer points at one of its atomics so the latest
    // available-object count is visible off-thread (diagnostics/calibration).
    void SetCountSink(std::atomic<UINT32>* sink) { m_countSink = sink; }

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
        ISpatialAudioObjectRenderStreamBase*, LONGLONG, UINT32 available) override {
        // The OS reapportions the shared dynamic-object pool when another
        // spatial client appears or departs. Record + log the new count so the
        // writer (and we, when calibrating) can see the pool breathe.
        if (m_countSink) m_countSink->store(available, std::memory_order_relaxed);
        LogMsg("[SpatialObjectWriter] Pool count change: %u dynamic objects available\n",
            available);
        return S_OK;
    }
private:
    std::atomic<ULONG> m_ref{1};
    std::atomic<UINT32>* m_countSink = nullptr;
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

void SpatialObjectWriter::ResetObjectPositionsToReference() {
    // Walk the canonical default-position table and write each one. Used by
    // paths (e.g. ProcessMultichannelObjects) that want to wipe out any
    // dynamic positioning a previous path (e.g. ProcessSpatialObjects)
    // applied to OBJ_VOCAL/OBJ_LEFT/RIGHT/OBJ_TOP_*.
    for (int i = 0; i < OBJ_COUNT; ++i) {
        const auto& p = kDefaultPositions[i];
        m_objects[i].position = {p.x, p.y, p.z};
    }
}

void SpatialObjectWriter::Shutdown() {
    // No background thread means nothing was ever started (a redundant plugin
    // instance that never processed audio) or we've already shut down. Return
    // quietly — no log spam, and no needless 100 ms settle-delay for a no-op.
    if (!m_thread.joinable()) return;

    Log("[SpatialObjectWriter] Shutdown requested\n");
    m_shutdownRequested = true;
    if (m_renderEvent) SetEvent(m_renderEvent);

    m_thread.join();

    // The thread tears the stream down on exit; this is a belt-and-braces
    // release for the case where the thread never started.
    TeardownStream();
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
    // Sanitize while copying: replace NaN/Inf with silence and SOFT-clip to
    // ±1.0 with a smooth knee. Hard clipping of peaks from our aggressive
    // band-gain paths (heights especially) produces harsh odd harmonics
    // that sound like rhythmic electrical crackle in sync with loud content.
    //
    // Soft knee formula: linear for |v| ≤ threshold, smooth asymptote to
    // ±1.0 above. At v = threshold, output = threshold (exact). At v → ∞,
    // output → ±1.0. Cheap (one divide per out-of-range sample, zero cost
    // for in-range samples).
    constexpr float kClipThreshold = 0.9f;
    constexpr float kClipKnee      = 1.0f - kClipThreshold; // 0.1
    // Master gain is applied BEFORE the soft knee so any boost-induced overload
    // is absorbed by the same smooth limiter rather than hard-clipping.
    const float gain = m_masterGain.load(std::memory_order_relaxed);
    float* dst = buf.data.data();
    for (uint32_t i = 0; i < frameCount; ++i) {
        float v = data[i];
        if (!std::isfinite(v)) {
            v = 0.0f;
        } else {
            v *= gain;
            if (v > kClipThreshold) {
                float over = v - kClipThreshold;
                v = kClipThreshold + kClipKnee * over / (over + kClipKnee);
            } else if (v < -kClipThreshold) {
                float over = -v - kClipThreshold;
                v = -kClipThreshold - kClipKnee * over / (over + kClipKnee);
            }
        }
        dst[i] = v;
    }
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

    // A SINGLE activation attempt. If it fails — most commonly because another
    // plugin instance (E-APO loads the plugin once for its editor view and
    // again in the audio engine) already holds the one spatial render stream —
    // we give up and let this instance fall back to its channel-based output.
    // Retrying in a loop here makes the two instances fight over the stream
    // endlessly, which manifests as unstable or absent spatial audio.
    if (!ActivateSpatialAudio()) {
        Log("[SpatialObjectWriter] Spatial audio activation FAILED\n");
        m_failed = true;
        m_active = false;
        TeardownStream();
        CoUninitialize();
        return;
    }

    Log("[SpatialObjectWriter] Spatial audio ACTIVE! Entering render loop.\n");
    m_active = true;

    // Enrol the render thread with the Multimedia Class Scheduler as a Pro Audio
    // task and raise it to real-time standing. Sound hygiene for any audio
    // render thread — it keeps the thread promptly serviced under system load.
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
    if (mmcssHandle) {
        AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_CRITICAL);
        Log("[SpatialObjectWriter] Render thread registered with MMCSS (Pro Audio)\n");
    } else {
        Log("[SpatialObjectWriter] WARNING: AvSetMmThreadCharacteristics failed: 0x%08lX\n",
            static_cast<unsigned long>(GetLastError()));
    }

    RenderLoop();

    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);

    m_active = false;
    TeardownStream();
    CoUninitialize();
}

void SpatialObjectWriter::TeardownStream() {
    // Release the COM graph in dependency order. Safe to call repeatedly and
    // on a partially-built stream (every member is null-checked by Reset()).
    // Does NOT touch the thread or m_shutdownRequested — Shutdown() owns those.
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
    notify->SetCountSink(&m_notifiedAvailable);

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
        if (m_shutdownRequested) return false;
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

    // Activate dynamic objects and record how many we actually hold — this is
    // the baseline the yield/reclaim machine measures contention against.
    m_objectTarget = ActivateObjects();
    Log("[SpatialObjectWriter] Activated %u/%d dynamic objects\n",
        m_objectTarget, OBJ_COUNT);

    // Start the stream
    hr = m_renderStream->Start();
    if (FAILED(hr)) {
        Log("[SpatialObjectWriter] Start failed: 0x%08lX\n",
            static_cast<unsigned long>(hr));
        return false;
    }

    return true;
}

UINT32 SpatialObjectWriter::ActivateObjects() {
    // (Re)activate dynamic objects on the already-live render stream. Used both
    // at startup and when re-claiming after a yield. Activating objects on an
    // existing stream is ordinary lifecycle — unlike re-activating the *stream*,
    // which is the manoeuvre that destabilised the two-instance contention.
    UINT32 activated = 0;
    for (int i = 0; i < OBJ_COUNT; ++i) {
        if (m_objects[i].active && m_objects[i].object) {
            ++activated;
            continue;
        }
        HRESULT hr = m_renderStream->ActivateSpatialAudioObject(
            AudioObjectType_Dynamic, m_objects[i].object.GetAddressOf());
        if (SUCCEEDED(hr) && m_objects[i].object) {
            m_objects[i].active = true;
            ++activated;
        } else {
            // Pool exhausted — a peer holds the rest. Stop and report what we have.
            m_objects[i].active = false;
            break;
        }
    }
    return activated;
}

void SpatialObjectWriter::ReleaseObjects() {
    // Hand our dynamic objects back to the shared pool, leaving the render
    // stream itself alive so a re-claim is cheap. The newcomer spatial client
    // can then draw the objects we relinquished.
    for (auto& obj : m_objects) {
        obj.object.Reset();
        obj.active = false;
    }
}

void SpatialObjectWriter::RenderLoop() {
    // Two postures. ACTIVE: we hold our objects and write audio. YIELDED: a peer
    // spatial client crowded the pool, so we let our objects go (m_active=false,
    // which steers the VST to plain stereo/channel output) and idle on the live
    // stream, watching for room to return.
    enum class Posture { Active, Yielded };
    Posture posture = Posture::Active;
    ULONGLONG contentionSince = 0;  // tick when a service shortfall began (0 = none)
    ULONGLONG roomSince       = 0;  // tick when the pool became roomy again (0 = none)
    bool eventStalled = false;      // render event has gone quiet (source buffering?)

    while (!m_shutdownRequested) {
        DWORD waitResult = WaitForSingleObject(m_renderEvent, kRenderWaitMs);
        if (m_shutdownRequested) break;

        // A timeout means the engine stopped signalling — the source has most
        // likely stalled (network re-buffer). We deliberately do NOT skip the
        // cycle here: running it writes fresh silence into every object so the
        // engine has nothing stale to repeat. Skipping (the old behaviour) left
        // the last non-silent buffers in place, which the engine looped as a
        // sustained buzz until playback resumed.
        if (waitResult == WAIT_TIMEOUT) {
            if (!eventStalled) {
                eventStalled = true;
                Log("[SpatialObjectWriter] Render event stalled — flushing silence\n");
            }
        } else if (eventStalled) {
            eventStalled = false;
            Log("[SpatialObjectWriter] Render event resumed\n");
        }

        UINT32 availableDynamic = 0;
        UINT32 frameCount = 0;
        HRESULT hr = m_renderStream->BeginUpdatingAudioObjects(&availableDynamic, &frameCount);
        if (FAILED(hr)) {
            if (hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED) {
                Log("[SpatialObjectWriter] Resources invalidated\n");
                break;  // device gone — exit; plugin falls back to channels
            }
            continue;
        }

        if (posture == Posture::Active) {
            // Write audio to each active object, counting how many the engine
            // actually serves a buffer this pass.
            UINT32 served = 0;
            for (int i = 0; i < OBJ_COUNT; ++i) {
                if (!m_objects[i].active || !m_objects[i].object) continue;

                auto& pos = m_objects[i].position;
                m_objects[i].object->SetPosition(pos.x, pos.y, pos.z);
                m_objects[i].object->SetVolume(1.0f);

                BYTE* buffer = nullptr;
                UINT32 bufferLength = 0;
                hr = m_objects[i].object->GetBuffer(&buffer, &bufferLength);
                if (FAILED(hr) || !buffer || bufferLength == 0) continue;
                ++served;

                // bufferLength is the AUTHORITATIVE byte count the spatial engine
                // handed back for this object this cycle. We must never write past
                // it: while the endpoint is mid-transition (monitor waking,
                // exclusive-mode handoff, GPU re-enumeration) the frameCount from
                // BeginUpdating and the per-object buffer can momentarily disagree.
                // An over-write here corrupts audiodg.exe's heap — which crashes
                // the entire Windows audio service. Clamp to whichever is smaller.
                const UINT32 framesAvail  = bufferLength / static_cast<UINT32>(sizeof(float));
                const UINT32 framesToFill = (frameCount < framesAvail) ? frameCount : framesAvail;
                if (framesToFill == 0) continue;

                bool hasData = false;
                {
                    std::lock_guard<std::mutex> lock(m_bufferMutex);
                    auto& objBuf = m_buffers[i];
                    if (objBuf.hasNewData.load(std::memory_order_acquire)) {
                        UINT32 copyFrames = (framesToFill < objBuf.validFrames)
                                          ? framesToFill : objBuf.validFrames;
                        std::memcpy(buffer, objBuf.data.data(), copyFrames * sizeof(float));
                        if (copyFrames < framesToFill) {
                            std::memset(buffer + copyFrames * sizeof(float), 0,
                                (framesToFill - copyFrames) * sizeof(float));
                        }
                        objBuf.hasNewData.store(false, std::memory_order_release);
                        hasData = true;
                    }
                }

                if (!hasData) {
                    std::memset(buffer, 0, framesToFill * sizeof(float));
                }
            }

            // Contention: if the engine no longer fields our full baseline of
            // objects, a peer spatial client is drawing from the shared pool.
            // Debounce so a momentary endpoint transition isn't mistaken for one.
            if (m_objectTarget > 0 && served < m_objectTarget) {
                const ULONGLONG now = GetTickCount64();
                if (contentionSince == 0) {
                    contentionSince = now;
                } else if (now - contentionSince >= kContentionDebounceMs) {
                    Log("[SpatialObjectWriter] Pool contended (served %u/%u, avail %u, "
                        "notified %u) — yielding objects to peer\n",
                        served, m_objectTarget, availableDynamic,
                        m_notifiedAvailable.load(std::memory_order_relaxed));
                    m_renderStream->EndUpdatingAudioObjects();
                    ReleaseObjects();
                    m_active = false;  // VST falls back to stereo/channel output
                    posture = Posture::Yielded;
                    contentionSince = 0;
                    roomSince = 0;
                    continue;
                }
            } else {
                contentionSince = 0;
            }
        } else {
            // YIELDED: holding no objects. Idle on the stream and watch the free
            // pool. When it can fit our whole baseline again — sustained past the
            // reclaim debounce — re-claim our objects and resume.
            if (m_objectTarget > 0 && availableDynamic >= m_objectTarget) {
                const ULONGLONG now = GetTickCount64();
                if (roomSince == 0) {
                    roomSince = now;
                } else if (now - roomSince >= kReclaimDebounceMs) {
                    UINT32 got = ActivateObjects();
                    if (got >= m_objectTarget) {
                        Log("[SpatialObjectWriter] Pool clear (avail %u) — reclaimed "
                            "%u/%u objects, resuming\n", availableDynamic, got, m_objectTarget);
                        m_active = true;
                        posture = Posture::Active;
                    } else {
                        // A peer beat us to the slots — let go and keep waiting.
                        ReleaseObjects();
                    }
                    roomSince = 0;
                }
            } else {
                roomSince = 0;
            }
        }

        hr = m_renderStream->EndUpdatingAudioObjects();
        if (FAILED(hr)) {
            if (hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED) {
                Log("[SpatialObjectWriter] Resources invalidated on EndUpdating\n");
                break;
            }
            Log("[SpatialObjectWriter] EndUpdatingAudioObjects failed: 0x%08lX\n",
                static_cast<unsigned long>(hr));
        }
    }
}

} // namespace MagicSpatial
