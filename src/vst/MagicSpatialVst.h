#pragma once

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <string>
#include "vst/VstDefs.h"
#include "processing/UpmixEngine.h"
#include "processing/ChannelLayout.h"
#include "processing/MultibandSplitter.h"
#include "processing/SpectralSeparator.h"
#include "processing/TransientDetector.h"
#include "processing/StereoCorrelationAnalyzer.h"
#include "processing/Decorrelator.h"
#include "processing/FeedbackDiffuser.h"
#include "processing/Biquad.h"
#include "audio/SpatialObjectWriter.h"
#include "core/Types.h"

namespace MagicSpatial {

// Physical speaker layouts supported by the plugin.
// Determines which output channels are active (rest are zeroed).
enum class SpeakerLayout {
    Layout_20,   // 2.0: Headphones — FL,FR only (all others folded down)
    Layout_51,   // 5.1: FL,FR,C,LFE,SL,SR (no heights)
    Layout_512,  // 5.1.2: FL,FR,C,LFE,SL,SR + TFL,TFR
    Layout_514,  // 5.1.4: FL,FR,C,LFE,SL,SR + TFL,TFR,TBL,TBR
    Layout_71,   // 7.1: FL,FR,C,LFE,SL,SR,BL,BR (no heights)
    Layout_712,  // 7.1.2: FL,FR,C,LFE,SL,SR,BL,BR + TFL,TFR
    Layout_714,  // 7.1.4: all 12 channels
};

// VST2 plugin: upmixes stereo/5.1/7.1 to Atmos bed for a given speaker layout.
// 12 inputs, 12 outputs. Unused input channels should be silent.
//
// Parameters:
//   0 - Mode:     Auto / Stereo / 5.1 / 7.1 / Passthrough
//   1 - Speakers:  2.0 / 5.1.2 / 5.1.4 / 7.1.2 / 7.1.4
//   2 - SurroundPos: Side / Rear
//   3 - Volume:   -12..+12 dB
//   4 - LfeCut:   40 / 65 / 80 / 100 / 120 Hz sub-bass crossover
//   5 - SubLevel: -12..+6 dB of sub weight around a level match with bypass
class MagicSpatialVst {
public:
    static constexpr int kNumInputs  = 12;
    static constexpr int kNumOutputs = 12;
    static constexpr int kNumParams  = 6;

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
    // Spawns the spatial writer on first audio processing. Deferred out of
    // effOpen so that ONLY the audio-engine instance activates spatial audio —
    // E-APO also loads the plugin in its editor GUI process, and a second
    // writer there would contend for the single per-endpoint spatial stream.
    void EnsureSpatialWriterStarted();
    InputLayout DetectLayoutFromInputs(float** inputs, VstInt32 sampleFrames);
    SpeakerLayout SpeakerLayoutFromParam() const;
    void ZeroUnusedChannels(float** outputs, VstInt32 sampleFrames, SpeakerLayout layout);
    // Linear multiplier derived from m_paramMasterGain (1.0 at the 0.5 centre).
    float MasterGainLinear() const;
    // Scale every output channel by the master gain (unity = cheap no-op).
    void ApplyMasterGainToOutputs(float** outputs, VstInt32 sampleFrames);
    void EditorRedraw();

    AEffect m_effect{};
    audioMasterCallback m_hostCallback;

    // Editor
    ERect m_editorRect{0, 0, 250, 340};
    void* m_editorHwnd = nullptr;  // HWND of our child window

    // Parameters
    float m_paramMode        = 0.0f;  // Auto
    float m_paramSpeakers    = 0.30f; // 5.1.2 (default)
    // Surround speaker physical position. 0.0 = Side (±90°, ITU reference),
    // 1.0 = Rear (±135°, useful when the user has placed their 5.1 surrounds
    // behind the listener position rather than to the sides). Only takes effect
    // when m_paramSpeakers selects 5.1, 5.1.2, or 5.1.4. Default is Rear since
    // that's the most common atypical setup we want to fix; users with true
    // side surrounds can switch back to Side.
    float m_paramSurroundPos = 1.0f;  // Rear (default)

    // Master output gain (shared-mode only). Normalized 0..1 maps linearly to
    // -12..+12 dB with 0.5 = unity (0 dB), so the default is a true no-op that
    // preserves the existing sound. Applied to every path we emit: spatial
    // objects (via SpatialObjectWriter::SetMasterGain, soft-clipped) and the
    // channel-based fallback outputs. Has NO effect on exclusive-mode or
    // bitstream-passthrough audio, which bypasses the Windows mixer entirely.
    float m_paramMasterGain = 0.5f;

    // Sub-bass crossover: the frequency below which the mid signal is handed
    // to the LFE bed. Set it to the front pair's own low-frequency limit so
    // the sub takes over exactly where the fronts stop. Combo encoding is
    // sel * 0.2 over kLfeCutChoices; decoded at the midpoints.
    static constexpr int   kLfeCutCount = 5;
    static constexpr float kLfeCutChoices[kLfeCutCount] = {40.0f, 65.0f, 80.0f, 100.0f, 120.0f};
    float m_paramLfeCut = 0.2f;  // 65 Hz
    int   LfeCutIndex() const;
    float LfeCutoffHz() const { return kLfeCutChoices[LfeCutIndex()]; }


    // Processing state
    UpmixEngine m_engine;
    float m_sampleRate = 48000.0f;
    VstInt32 m_blockSize = 4096;
    bool m_engineInitialized = false;
    InputLayout m_currentLayout = InputLayout::Unknown;

    // Layout-detection hysteresis. Once committed to a layout, we require the
    // raw signal-energy detection to report a DIFFERENT layout for
    // kLayoutHysteresisBlocks consecutive blocks before switching. Prevents
    // rapid flip-flop between stereo and multichannel code paths when audio
    // briefly quiets on some channels (e.g. during a focus change, a quiet
    // game scene, or Windows-level audio routing shifts). Each switch would
    // otherwise cause DSP-state discontinuities perceived as crackles.
    InputLayout m_committedLayout = InputLayout::Stereo;
    InputLayout m_pendingLayout   = InputLayout::Stereo;
    int m_pendingSamples = 0; // samples of consistent pending-layout evidence
    // Time-based hysteresis: commit to a new layout after this many samples of
    // consistent detection. Keeps responsiveness stable across host block sizes
    // (20-block constant drifted between 53 ms @ 128-sample and 1.7 s @ 4096).
    static constexpr int kLayoutHysteresisMs = 200;
    // Releasing a multichannel layout back to stereo is held far longer. A
    // game that drops its native surround stream for a second between maps
    // would otherwise drag any concurrent stereo audio (music, a video) into
    // the upmix path and straight back out again. Acquiring a layout stays on
    // the short constant above, so surround still engages promptly.
    static constexpr int kLayoutReleaseHoldMs = 5000;

    // In-process spatial object output via ISpatialAudioClient
    SpatialObjectWriter m_spatialWriter;
    // Guards one-time spawn of the spatial writer. Atomic because it is touched
    // from both the control thread (effMainsChanged) and the audio thread
    // (ProcessReplacing) — whichever processing signal arrives first.
    std::atomic<bool> m_spatialInitAttempted{false};

    // Spatial decomposition DSP (used when spatial objects are active)
    SpectralSeparator m_spatialSeparator;
    MultibandSplitter m_spatialSplitter;
    StereoCorrelationAnalyzer m_spatialCorrelation;
    TransientDetector m_spatialTransients;
    // --- Bass management at LfeCutoffHz(), shared by both spatial paths ---
    // LR4 lowpass/highpass pairs, complementary so the acoustic sum is flat.
    // Stereo path: OBJ_SUBBASS carries the lowpassed mid, the fronts are
    // highpassed. Multichannel path: every non-LFE channel is highpassed to
    // its object and its lowpassed remainder is summed into the LFE bed, so a
    // game's rear or height bass reaches the sub even where the renderer's
    // own bass management does not. Each pair is complementary, so every
    // frequency is reproduced exactly once and the acoustic sum is flat.
    //
    // The multichannel path uses TWO corners. Its front three hand over at
    // LfeCutoffHz() like the stereo fronts, while its surrounds and heights -
    // slots 4 and up in every layout accepted - hand over at the higher
    // kWashCutHz, because those are smaller drivers crossed higher and the
    // octave between is excursion they spend without making sound. All
    // coefficients are re-designed on the audio thread when the selector moves;
    // m_lfeCutoffApplied records the frequency currently held.
    BiquadFilter m_spatialLfeLowpass[2];
    BiquadFilter m_frontHp[2][2];              // [L/R][stage]
    BiquadFilter m_mcHp[kNumInputs][2];
    BiquadFilter m_mcLp[kNumInputs][2];
    std::vector<float> m_mcHpOut;
    std::vector<float> m_mcBedSum;
    float m_lfeCutoffApplied = 0.0f;
    void ApplyLfeCutoff();
    // Level at which redirected bass joins the LFE bed. This was 0.316, a 10 dB
    // cut to leave room for the +10 dB lift an LFE channel receives in Dolby
    // Digital, DTS and their kin. That lift never arrives here. It belongs to
    // bitstream formats, where the decoder applies it on the way out; an
    // ISpatialAudioClient bed takes linear PCM and there is no decode step to
    // apply any such convention, so what we write is what the sub plays.
    //
    // The listening bears it out. With the sub carrying everything below the
    // mains' corner the bass measured flat on the assumption of a lift and yet
    // sounded a full measure shy, and that same 10 dB deficit accounts for
    // every report across three rounds of tuning. Unity it is.
    static constexpr float kBassRedirectGain = 1.0f;
    // Sub weight above a level match with the effect bypassed. Parity itself is
    // derived, not guessed: see the OBJ_SUBBASS submission. This is the single
    // knob for subwoofer level in the stereo path, and 0 dB means the sub puts
    // exactly as much bass into the room as the untouched stereo pair did.
    // Combo encoding is sel / 6 over kSubLevelChoicesDb; decoded at the
    // midpoints.
    // The range spans both directions on purpose. Match is the derived parity,
    // and it now rests on kBassRedirectGain being right; should that judgement
    // prove wrong, the negative steps reach back to where the sub sat before
    // without another rebuild.
    static constexpr int   kSubLevelCount = 7;
    static constexpr float kSubLevelChoicesDb[kSubLevelCount] =
        {-12.0f, -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f};
    float m_paramSubLevel = 4.0f / 6.0f;  // Match
    int   SubLevelIndex() const;
    float SubLevelDb() const { return kSubLevelChoicesDb[SubLevelIndex()]; }
    float SubLevelLinear() const { return std::pow(10.0f, SubLevelDb() / 20.0f); }
    Decorrelator m_spatialDecorr[8];
    bool m_spatialDspInitialized = false;

    // Pre-allocated scratch buffers (avoids heap allocation on audio thread)
    std::vector<float> m_sBandL[4], m_sBandR[4];
    std::vector<float> m_sSide0, m_sSide1, m_sSide2, m_sSide3;
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

    // --- Feature 1: Early-reflection pre-delay on rear objects ---
    // Ring-buffer delay applied to OBJ_SIDE and OBJ_BACK before submit,
    // creating a perceptual gap between direct front image and surround wash.
    static constexpr int kRearPreDelayMs = 5;
    static constexpr int kRearPreDelayMaxSamples = 1024; // >= ceil(15ms @ 48k)
    std::vector<float> m_rearDelayRing[4]; // [0]=sideL [1]=sideR [2]=backL [3]=backR
    int m_rearDelayLength = 0;
    int m_rearDelaySidePos = 0;
    int m_rearDelayBackPos = 0;

    // --- Feature 4: Feedback diffusion on rear objects ---
    FeedbackDiffuser m_rearDiffuser[4]; // [0]=sideL [1]=sideR [2]=backL [3]=backR

    // --- Feature 2: Correlation-adaptive spatial extension gain ---
    float m_smoothSpatialExtGain = 0.60f;
    static constexpr float kSpatialExtGainSmoothing = 0.04f; // ~200 ms ramp
    static constexpr float kSpatialExtGainMin = 0.45f;
    static constexpr float kSpatialExtGainMax = 0.75f;

    // --- Feature 5: Low-mid envelopment feed ---
    // Bass and low-mids are mono in nearly every mix, so the L-R side signal
    // that drives every surround/height object is empty below ~300 Hz and the
    // wash carries presence only. This feed takes the 100-250 Hz body of the
    // MID signal, decorrelates each copy with long delays and low-break
    // allpasses (the stock presets are transparent at these wavelengths), and
    // adds it IN PHASE to the sides, backs and heights. In-phase copies survive
    // the renderer's bass management and never null at the seat.
    BiquadFilter m_lowMidHp[2];   // LR4 highpass at kLowMidEnvLowHz
    BiquadFilter m_lowMidLp[2];   // LR4 lowpass at kLowMidEnvHighHz
    Decorrelator m_lowMidDecorr[8]; // [0]SL [1]SR [2]BL [3]BR [4]TFL [5]TFR [6]TBL [7]TBR
    std::vector<float> m_sLowMid;
    static constexpr float kLowMidEnvLowHz  = 100.0f;
    static constexpr float kLowMidEnvHighHz = 250.0f;
    // Gain relative to the delayed mid, before spatialExtGain. Surround pairs
    // receive the full amount, heights half.
    static constexpr float kLowMidEnvGain       = 0.55f;
    static constexpr float kLowMidEnvHeightGain = 0.5f;

    // --- Wash bass management ---
    // The surround and height speakers are smaller than the fronts and crossed
    // higher, so whatever the wash carries below kWashCutHz is excursion those
    // drivers spend without making sound - and a small driver pushed on notes
    // it cannot reproduce hands back intermodulation, which reads as harshness
    // rather than as bass. Every wash object is highpassed here and the summed
    // remainder joins the LFE bed, so that energy arrives from the sub instead
    // of being lost in the surrounds. The multichannel path has always done
    // exactly this for its own channels; the stereo path did it only for the
    // fronts.
    //
    // Antiphase content cancels in the sum, and that is correct rather than a
    // loss: the side signal is inverted between left and right, and at these
    // wavelengths a symmetric pair cancels it at the seat in any case. The
    // low-mid envelopment feed is added in phase, so it survives, and it is
    // what mostly makes the journey to the sub.
    //
    // The heights have their own corner. The up-firing pair reaches 100 Hz
    // where the satellites stop at 120, so the height objects hand over at
    // kHeightCutHz and their redirect is lowpassed there, separately, so each
    // frequency is still reproduced exactly once.
    static constexpr float kWashCutHz   = 120.0f;   // sides and backs
    static constexpr float kHeightCutHz = 100.0f;   // top objects
    BiquadFilter m_washHp[8][2];          // [object][stage]; objects 4-7 at kHeightCutHz
    BiquadFilter m_washLp[2];             // on the summed surround redirect
    BiquadFilter m_washLpHeight[2];       // on the summed height redirect
    std::vector<float> m_sWashLow;        // surround wash sum awaiting its lowpass
    std::vector<float> m_sWashLowHeight;  // height wash sum awaiting its lowpass
    std::vector<float> m_sSubOut;   // sub feed, held until the wash folds in

    // --- Spatial energy budget ---
    // The upmix sprays one stereo pair across up to eight further speakers.
    // Summed around the room that is a great deal more acoustic power than the
    // front pair alone would have made, and because every surround and height
    // object is built from the L-R SIDE signal - which is mid and treble heavy,
    // bass being mono in nearly every mix - the surplus lands almost entirely
    // above the bass. The image lifts and brightens, and the sub, still just
    // one driver's worth of output, is left outgunned.
    //
    // These two shares divide the stereo bypass power between the direct front
    // pair and the surround/height wash, so the total in the room comes back to
    // what the fronts alone would have given. They should sum to 1. The sub is
    // deliberately outside the budget: holding it still while everything around
    // it comes down is exactly how it regains its footing.
    static constexpr float kDirectShare = 0.75f;
    static constexpr float kWashShare   = 0.25f;
    // The wash cannot be measured until it has been built, so each block is
    // scaled by the ratio the previous block computed and the result is
    // smoothed over kWashNormTau. Ceilinged at unity so this only ever reins
    // in: near-mono material, whose side signal is already faint, keeps the
    // voicing it has rather than being hauled up to fill the budget.
    static constexpr float kWashNormMin = 0.10f;
    static constexpr float kWashNormTau = 0.5f;  // seconds
    float m_washNormGain = 1.0f;

    // The budget is divided BAND BY BAND as well as in total. A broadband quota
    // is drawn from the whole spectrum, which in music is dominated by the bass,
    // yet the wash spends it wherever the side signal happens to live - and in
    // most mixes that is the presence band, since the low end is mono and lead
    // vocals are centred while reverb and wide synths are not. Measured on real
    // programme, a broadband quota alone lands about +2.7 dB at 4 kHz against a
    // stereo bypass while the extremes sit near +1, which is heard as the image
    // brightening.
    //
    // m_washBandGain corrects that where the side bands are formed, which is
    // where kBandBalance already applies a per-band multiply, so it costs one
    // further scalar. Each band is weighed against ITS OWN bypass energy rather
    // than the spectrum's, and the set is renormalised afterwards to leave the
    // total untouched - that keeps m_washNormGain above in charge of the level
    // and inside the range it already occupied, rather than driving it into its
    // ceiling and quietly costing envelopment.
    //
    // kBandBalance and this do different jobs. kBandBalance flattens the wash's
    // TRANSFER FUNCTION, and measurement says it manages that to within 1.3 dB
    // from 250 Hz to 16 kHz. This flattens the wash's ENERGY against the
    // programme, which no fixed constant can do, because it moves with the mix.
    static constexpr float kWashBandMin = 0.25f;
    static constexpr float kWashBandMax = 2.50f;
    float m_washBandGain[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // --- Feature 3: Spectral-variance ambience extraction ---
    int m_ambBinCount[4] = {0, 0, 0, 0};        // precomputed bin counts per band
    float m_smoothAmbFactor[4] = {0.f, 0.f, 0.f, 0.f};
    static constexpr float kAmbFactorSmoothing = 0.06f;

    void InitSpatialDsp();
    void ProcessSpatialObjects(float** inputs, float** outputs, VstInt32 sampleFrames);

    // When m_paramSpeakers is 5.1/5.1.2/5.1.4 AND m_paramSurroundPos is "Rear",
    // override OBJ_SIDE_LEFT/RIGHT positions to the back-surround coordinates
    // (±0.707, 0, 0.707). This causes the Dolby renderer to map "side" content
    // onto the user's physically rear-placed surround speakers. Cheap (2
    // SetObjectPosition calls); safe to call every audio block.
    void ApplySurroundPositionOverride();

    // Multichannel-to-object promotion path. For 5.1/7.1/Passthrough input,
    // each channel is promoted to a positioned Atmos object at its ITU
    // reference position with zero added latency; the only processing is
    // the bass management above. The Dolby renderer then maps those objects
    // to whatever physical speakers exist. Used only when
    // m_spatialWriter.IsActive().
    void ProcessMultichannelObjects(float** inputs, float** outputs,
                                    VstInt32 sampleFrames, InputLayout layout);

    // Pre-zeroed buffer reused as the "silence" source for OBJ_* slots that
    // a given multichannel layout does not feed. Sized in dispatch
    // when block size is known.
    std::vector<float> m_silenceBuffer;

    // --- Multichannel-path stereo-aware extraction ---
    //
    // When a stereo source (video, music) is mixed INTO a multichannel stream
    // (e.g. a game playing 5.1 while a video plays stereo in another window),
    // Windows sums both into the same endpoint channels. To preserve the
    // stereo content's spatialization, the multichannel path runs the
    // SpectralSeparator on FL/FR and delays channels 2-11 by kFftSize samples
    // so everything stays time-aligned.
    //
    // Ring buffers: channels 2..11 only (0/1 delayed by the separator itself).
    static constexpr int kMcDelaySize = SpectralSeparator::kFftSize;
    std::vector<float> m_mcDelayRing[kNumInputs];   // size kMcDelaySize
    std::vector<float> m_mcDelayed[kNumInputs];     // size maxFrames
    int m_mcDelayPos = 0;
};

} // namespace MagicSpatial
