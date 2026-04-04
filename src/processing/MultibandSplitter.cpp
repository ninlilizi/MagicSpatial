#include "processing/MultibandSplitter.h"

namespace MagicSpatial {

void MultibandSplitter::Initialize(float sampleRate, uint32_t maxFrameCount) {
    // Design LR4 crossover coefficients (each is Butterworth 2nd-order, cascaded twice)
    auto lp200 = DesignLowpass(kCrossover1, sampleRate);
    auto hp200 = DesignHighpass(kCrossover1, sampleRate);
    auto lp2k  = DesignLowpass(kCrossover2, sampleRate);
    auto hp2k  = DesignHighpass(kCrossover2, sampleRate);
    auto lp8k  = DesignLowpass(kCrossover3, sampleRate);
    auto hp8k  = DesignHighpass(kCrossover3, sampleRate);

    // Left channel
    m_left.lp200.SetCoeffs(lp200);  m_left.hp200.SetCoeffs(hp200);
    m_left.lp2k.SetCoeffs(lp2k);   m_left.hp2k.SetCoeffs(hp2k);
    m_left.lp8k.SetCoeffs(lp8k);   m_left.hp8k.SetCoeffs(hp8k);

    // Right channel (same coefficients, separate state)
    m_right.lp200.SetCoeffs(lp200); m_right.hp200.SetCoeffs(hp200);
    m_right.lp2k.SetCoeffs(lp2k);  m_right.hp2k.SetCoeffs(hp2k);
    m_right.lp8k.SetCoeffs(lp8k);  m_right.hp8k.SetCoeffs(hp8k);

    m_scratch1.resize(maxFrameCount);
    m_scratch2.resize(maxFrameCount);
}

void MultibandSplitter::Process(const float* inputL, const float* inputR,
                                 uint32_t frameCount,
                                 float* bandL[kNumBands], float* bandR[kNumBands]) {
    // --- Left channel ---
    // Split at 200Hz: band0 = LP, temp = HP
    m_left.lp200.Process(inputL, bandL[0], frameCount, m_scratch1.data());
    m_left.hp200.Process(inputL, m_scratch2.data(), frameCount, m_scratch1.data());

    // Split HP200 output at 2kHz: band1 = LP, temp = HP
    m_left.lp2k.Process(m_scratch2.data(), bandL[1], frameCount, m_scratch1.data());
    m_left.hp2k.Process(m_scratch2.data(), m_scratch1.data(), frameCount, bandL[3]); // borrow bandL[3] as scratch

    // Split HP2k output at 8kHz: band2 = LP, band3 = HP
    m_left.lp8k.Process(m_scratch1.data(), bandL[2], frameCount, bandL[3]);
    m_left.hp8k.Process(m_scratch1.data(), bandL[3], frameCount, m_scratch2.data());

    // --- Right channel ---
    m_right.lp200.Process(inputR, bandR[0], frameCount, m_scratch1.data());
    m_right.hp200.Process(inputR, m_scratch2.data(), frameCount, m_scratch1.data());

    m_right.lp2k.Process(m_scratch2.data(), bandR[1], frameCount, m_scratch1.data());
    m_right.hp2k.Process(m_scratch2.data(), m_scratch1.data(), frameCount, bandR[3]);

    m_right.lp8k.Process(m_scratch1.data(), bandR[2], frameCount, bandR[3]);
    m_right.hp8k.Process(m_scratch1.data(), bandR[3], frameCount, m_scratch2.data());
}

void MultibandSplitter::Reset() {
    m_left.Reset();
    m_right.Reset();
}

} // namespace MagicSpatial
