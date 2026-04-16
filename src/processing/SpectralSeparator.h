#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace MagicSpatial {

// Frequency-domain stereo source separator.
//
// Performs a streaming STFT on a stereo pair, computes a per-bin "centre mask"
// from the phase/magnitude similarity of L and R (Avendano & Jot 2004 style),
// and produces three time-domain output streams:
//
//   - delayedL, delayedR : the input L/R, delayed by exactly kFftSize samples
//                          so they line up with the centre output.
//   - centerMono         : the per-bin centre-locked content, summed to mono.
//                          Subtracting this from delayedL/delayedR yields clean
//                          stereo residuals with the phantom centre peeled out
//                          WITHOUT removing neighbouring-frequency content.
//
// Algorithmic latency: kFftSize samples (= 1024 at 48 kHz -> ~21.3 ms).
// No lookahead beyond that; fully causal.
class SpectralSeparator {
public:
    static constexpr int kFftSize     = 1024;
    static constexpr int kHopSize     = 512;   // 50% overlap
    static constexpr int kNumBins     = kFftSize / 2 + 1;
    static constexpr int kLatencySamples = kFftSize;

    void Initialize(float sampleRate, uint32_t maxFrameCount);

    // Process a block. All output buffers must be at least frameCount samples.
    void Process(const float* inputL, const float* inputR, uint32_t frameCount,
                 float* outDelayedL, float* outDelayedR, float* outCenter);

    void Reset();

private:
    // --- FFT machinery (radix-2 Cooley-Tukey) ---
    int m_logFftSize = 0;
    std::vector<uint32_t> m_bitReverse;                 // size kFftSize
    std::vector<std::complex<float>> m_twiddles;        // size kFftSize/2

    void FFTForward(std::complex<float>* data);
    void FFTInverse(std::complex<float>* data);

    // --- STFT state ---
    std::vector<float> m_window;                        // Hann, size kFftSize

    // Circular input history used to feed each FFT frame.
    std::vector<float> m_historyL;                      // size kFftSize
    std::vector<float> m_historyR;
    int m_historyPos = 0;                               // next write position
    int m_hopCounter = 0;                               // counts up to kHopSize

    // FFT scratch buffers.
    std::vector<std::complex<float>> m_fftBufL;
    std::vector<std::complex<float>> m_fftBufR;
    std::vector<std::complex<float>> m_fftBufC;         // centre bins before IFFT

    // Per-bin temporally smoothed centre mask.
    std::vector<float> m_smoothMask;                    // size kNumBins
    float m_smoothAlpha = 0.0f;                         // 0..1, closer to 1 = smoother

    // First FFT bin whose centre frequency is at or above the bass-extract
    // cutoff. Bins below this index have their mask forced to zero so bass
    // content stays in the residual L/R streams (i.e. continues to play
    // through both front speakers as the original phantom image) instead of
    // being concentrated into OBJ_VOCAL — which would render through a
    // single centre channel or phantom and sound noticeably thinner.
    int m_bassMaskCutoffBin = 0;

    // --- Spectral-variance ambience tracking ---
    // Per-bin running mean and mean-of-squares of the mid-channel magnitude.
    // Variance = E[X²] - E[X]² gives a coefficient of variation that
    // distinguishes reverb tails / ambient content (high variance) from
    // steady-state direct signal (low variance).
    std::vector<float> m_binMean;            // size kNumBins
    std::vector<float> m_binMeanSq;          // size kNumBins
    std::vector<float> m_ambienceWeight;     // size kNumBins, normalized [0,1]
    float m_varianceAlpha = 0.0f;            // smoothing (e.g. ~500 ms tau)

public:
    // Per-bin ambience weights [0..1], valid after Process(). Size = kNumBins.
    const float* GetAmbienceWeights() const { return m_ambienceWeight.data(); }

private:

    // Per-frame mask after cross-bin smoothing (applied on top of the
    // per-bin time smoothing). Cross-bin smoothing suppresses the classic
    // "musical noise" artifact where adjacent FFT bins with very different
    // mask values cause micro-discontinuities across frequency — audible
    // as a faint content-tracking buzz in the residuals (delayedL/R − C).
    std::vector<float> m_frameMask;                     // size kNumBins

    // Overlap-add accumulator for the centre output (size kFftSize, linear).
    std::vector<float> m_accCenter;

    // FIFO of finalized centre samples ready to be handed to the caller.
    // Size is kHopSize * 2 so that a full hop can be pushed even if the caller
    // drained the queue asynchronously across block boundaries.
    std::vector<float> m_readyCenter;
    int m_readyRead  = 0;
    int m_readyWrite = 0;
    int m_readyCount = 0;

    // Delay lines for the pass-through L/R (kFftSize-sample delay, matching OLA).
    std::vector<float> m_delayL;                        // size kFftSize
    std::vector<float> m_delayR;
    int m_delayPos = 0;

    // Do one FFT frame: read the latest kFftSize samples from history, mask,
    // IFFT, OLA into m_accCenter, push the next kHopSize finalized samples
    // into m_readyCenter.
    void ProcessFrame();
};

} // namespace MagicSpatial
