#pragma once

#include "processing/Biquad.h"
#include <cstdint>
#include <vector>

namespace MagicSpatial {

// Splits a stereo signal into 4 frequency bands using Linkwitz-Riley 4th-order crossovers.
// LR4 = 2 cascaded Butterworth 2nd-order filters per path, giving flat magnitude sum.
// Bands: [0] < 200Hz, [1] 200Hz-2kHz, [2] 2kHz-8kHz, [3] > 8kHz
class MultibandSplitter {
public:
    static constexpr int kNumBands = 4;
    static constexpr float kCrossover1 = 200.0f;
    static constexpr float kCrossover2 = 2000.0f;
    static constexpr float kCrossover3 = 8000.0f;

    void Initialize(float sampleRate, uint32_t maxFrameCount);

    // Split stereo into 4 bands. bandL/bandR are arrays of 4 pointers,
    // each pointing to a buffer of at least frameCount floats.
    void Process(const float* inputL, const float* inputR, uint32_t frameCount,
                 float* bandL[kNumBands], float* bandR[kNumBands]);

    void Reset();

private:
    // LR4 filter: two cascaded Butterworth 2nd-order biquads
    struct LR4Filter {
        BiquadFilter stage1;
        BiquadFilter stage2;

        void SetCoeffs(const BiquadCoeffs& c) {
            stage1.SetCoeffs(c);
            stage2.SetCoeffs(c);
        }
        void Process(const float* in, float* out, uint32_t count, float* scratch) {
            stage1.Process(in, scratch, count);
            stage2.Process(scratch, out, count);
        }
        void Reset() {
            stage1.Reset();
            stage2.Reset();
        }
    };

    // One channel's crossover: 3 split points = 6 LR4 filters
    struct ChannelCrossover {
        LR4Filter lp200, hp200;
        LR4Filter lp2k,  hp2k;
        LR4Filter lp8k,  hp8k;
        void Reset() {
            lp200.Reset(); hp200.Reset();
            lp2k.Reset();  hp2k.Reset();
            lp8k.Reset();  hp8k.Reset();
        }
    };

    ChannelCrossover m_left;
    ChannelCrossover m_right;

    // Scratch buffers for intermediate cascaded splitting
    std::vector<float> m_scratch1;
    std::vector<float> m_scratch2;
};

} // namespace MagicSpatial
