#include "processing/SpectralSeparator.h"

#include <algorithm>
#include <cmath>

namespace MagicSpatial {

namespace {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kEps = 1e-12f;
    // Temporal smoothing time constant for the per-bin mask. Long enough to
    // kill musical noise, short enough to track vocal onsets.
    constexpr float kMaskTauSeconds = 0.030f; // 30 ms
}

void SpectralSeparator::Initialize(float sampleRate, uint32_t /*maxFrameCount*/) {
    // --- FFT tables ---
    m_logFftSize = 0;
    while ((1 << m_logFftSize) < kFftSize) ++m_logFftSize;

    m_bitReverse.assign(kFftSize, 0u);
    for (int i = 0; i < kFftSize; ++i) {
        uint32_t rev = 0;
        uint32_t x = static_cast<uint32_t>(i);
        for (int b = 0; b < m_logFftSize; ++b) {
            rev = (rev << 1) | (x & 1u);
            x >>= 1;
        }
        m_bitReverse[i] = rev;
    }

    m_twiddles.assign(kFftSize / 2, std::complex<float>(0.0f, 0.0f));
    for (int i = 0; i < kFftSize / 2; ++i) {
        float angle = -2.0f * kPi * static_cast<float>(i) / static_cast<float>(kFftSize);
        m_twiddles[i] = std::complex<float>(std::cos(angle), std::sin(angle));
    }

    // --- Hann analysis window (synthesis is rectangular; 50% OLA sums to 1) ---
    m_window.assign(kFftSize, 0.0f);
    for (int n = 0; n < kFftSize; ++n) {
        m_window[n] = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(n) / static_cast<float>(kFftSize)));
    }

    // --- Buffers ---
    m_historyL.assign(kFftSize, 0.0f);
    m_historyR.assign(kFftSize, 0.0f);
    m_fftBufL.assign(kFftSize, std::complex<float>(0.0f, 0.0f));
    m_fftBufR.assign(kFftSize, std::complex<float>(0.0f, 0.0f));
    m_fftBufC.assign(kFftSize, std::complex<float>(0.0f, 0.0f));
    m_smoothMask.assign(kNumBins, 0.0f);
    m_accCenter.assign(kFftSize, 0.0f);
    m_readyCenter.assign(kHopSize * 2, 0.0f);
    m_delayL.assign(kFftSize, 0.0f);
    m_delayR.assign(kFftSize, 0.0f);

    // --- Smoothing coefficient at frame rate ---
    float hopSeconds = static_cast<float>(kHopSize) / sampleRate;
    m_smoothAlpha = std::exp(-hopSeconds / kMaskTauSeconds);

    Reset();
}

void SpectralSeparator::Reset() {
    std::fill(m_historyL.begin(), m_historyL.end(), 0.0f);
    std::fill(m_historyR.begin(), m_historyR.end(), 0.0f);
    std::fill(m_accCenter.begin(), m_accCenter.end(), 0.0f);
    std::fill(m_readyCenter.begin(), m_readyCenter.end(), 0.0f);
    std::fill(m_smoothMask.begin(), m_smoothMask.end(), 0.0f);
    std::fill(m_delayL.begin(), m_delayL.end(), 0.0f);
    std::fill(m_delayR.begin(), m_delayR.end(), 0.0f);
    m_historyPos = 0;
    m_hopCounter = 0;
    m_readyRead = 0;
    m_readyWrite = 0;
    m_readyCount = 0;
    m_delayPos = 0;
}

void SpectralSeparator::Process(const float* inputL, const float* inputR, uint32_t frameCount,
                                 float* outDelayedL, float* outDelayedR, float* outCenter) {
    const int readySize = static_cast<int>(m_readyCenter.size());

    for (uint32_t i = 0; i < frameCount; ++i) {
        // --- Delay line: fetch-then-store yields exactly kFftSize-sample delay ---
        outDelayedL[i] = m_delayL[m_delayPos];
        outDelayedR[i] = m_delayR[m_delayPos];
        m_delayL[m_delayPos] = inputL[i];
        m_delayR[m_delayPos] = inputR[i];
        m_delayPos = (m_delayPos + 1) % kFftSize;

        // --- Centre from the OLA queue (silence during initial kFftSize warmup) ---
        if (m_readyCount > 0) {
            outCenter[i] = m_readyCenter[m_readyRead];
            m_readyRead = (m_readyRead + 1) % readySize;
            --m_readyCount;
        } else {
            outCenter[i] = 0.0f;
        }

        // --- Push input into circular history ---
        m_historyL[m_historyPos] = inputL[i];
        m_historyR[m_historyPos] = inputR[i];
        m_historyPos = (m_historyPos + 1) % kFftSize;

        // --- Fire a frame every kHopSize input samples ---
        ++m_hopCounter;
        if (m_hopCounter >= kHopSize) {
            m_hopCounter = 0;
            ProcessFrame();
        }
    }
}

void SpectralSeparator::ProcessFrame() {
    // 1. Copy the latest kFftSize samples from the circular history into the
    //    FFT buffers, applying the Hann analysis window in the same pass.
    for (int n = 0; n < kFftSize; ++n) {
        int idx = (m_historyPos + n) % kFftSize;
        float wn = m_window[n];
        m_fftBufL[n] = std::complex<float>(m_historyL[idx] * wn, 0.0f);
        m_fftBufR[n] = std::complex<float>(m_historyR[idx] * wn, 0.0f);
    }

    // 2. Forward FFT both channels.
    FFTForward(m_fftBufL.data());
    FFTForward(m_fftBufR.data());

    // 3. Per-bin centre mask and extraction for unique bins [0, kNumBins).
    //    phi   = 2·Re{L·conj(R)} / (|L|² + |R|²)   in [-1,1]  (phase alignment)
    //    bal   = sqrt(min/max of energies)          in [0,1]   (amplitude match)
    //    mask  = max(0, phi) · bal                              (centred confidence)
    //    C(k)  = mask · (L + R) / 2                             (mono sum scaled by mask)
    for (int k = 0; k < kNumBins; ++k) {
        auto L = m_fftBufL[k];
        auto R = m_fftBufR[k];
        float ampL2 = L.real() * L.real() + L.imag() * L.imag();
        float ampR2 = R.real() * R.real() + R.imag() * R.imag();
        float crossRe = L.real() * R.real() + L.imag() * R.imag();

        float phi = 2.0f * crossRe / (ampL2 + ampR2 + kEps);
        if (phi < 0.0f) phi = 0.0f;

        float ampMin = (ampL2 < ampR2) ? ampL2 : ampR2;
        float ampMax = (ampL2 > ampR2) ? ampL2 : ampR2;
        float balance = std::sqrt(ampMin / (ampMax + kEps));

        float raw = phi * balance;

        // One-pole temporal smoothing at frame rate.
        m_smoothMask[k] = m_smoothAlpha * m_smoothMask[k] + (1.0f - m_smoothAlpha) * raw;
        float mask = m_smoothMask[k];

        std::complex<float> M = (L + R) * 0.5f;
        m_fftBufC[k] = M * mask;
    }

    // 4. Hermitian mirror the negative frequencies so the IFFT yields a real signal.
    for (int k = 1; k < kFftSize / 2; ++k) {
        m_fftBufC[kFftSize - k] = std::conj(m_fftBufC[k]);
    }

    // 5. Inverse FFT (time-domain centre frame).
    FFTInverse(m_fftBufC.data());

    // 6. Overlap-add into the centre accumulator.
    for (int n = 0; n < kFftSize; ++n) {
        m_accCenter[n] += m_fftBufC[n].real();
    }

    // 7. Push the first kHopSize (now-finalized) samples into the ready FIFO.
    const int readySize = static_cast<int>(m_readyCenter.size());
    for (int n = 0; n < kHopSize; ++n) {
        m_readyCenter[m_readyWrite] = m_accCenter[n];
        m_readyWrite = (m_readyWrite + 1) % readySize;
    }
    m_readyCount += kHopSize;

    // 8. Shift the accumulator left by kHopSize and zero the new tail.
    for (int n = 0; n < kFftSize - kHopSize; ++n) {
        m_accCenter[n] = m_accCenter[n + kHopSize];
    }
    for (int n = kFftSize - kHopSize; n < kFftSize; ++n) {
        m_accCenter[n] = 0.0f;
    }
}

// -------- Radix-2 Cooley-Tukey FFT (in-place) --------

void SpectralSeparator::FFTForward(std::complex<float>* data) {
    // Bit-reverse permutation.
    for (int i = 0; i < kFftSize; ++i) {
        uint32_t j = m_bitReverse[i];
        if (static_cast<uint32_t>(i) < j) {
            std::swap(data[i], data[j]);
        }
    }
    // Butterflies.
    for (int half = 1; half < kFftSize; half <<= 1) {
        int fullSize = half << 1;
        int twiddleStride = kFftSize / fullSize;
        for (int group = 0; group < kFftSize; group += fullSize) {
            for (int j = 0; j < half; ++j) {
                std::complex<float> w = m_twiddles[j * twiddleStride];
                std::complex<float> t = data[group + j + half] * w;
                std::complex<float> u = data[group + j];
                data[group + j]        = u + t;
                data[group + j + half] = u - t;
            }
        }
    }
}

void SpectralSeparator::FFTInverse(std::complex<float>* data) {
    // IFFT via conjugate-FFT-conjugate trick, with 1/N scaling.
    for (int i = 0; i < kFftSize; ++i) {
        data[i] = std::conj(data[i]);
    }
    FFTForward(data);
    float inv = 1.0f / static_cast<float>(kFftSize);
    for (int i = 0; i < kFftSize; ++i) {
        data[i] = std::conj(data[i]) * inv;
    }
}

} // namespace MagicSpatial
