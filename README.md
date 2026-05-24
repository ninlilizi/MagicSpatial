# MagicSpatial

An [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) plugin that turns stereo audio into Dolby Atmos spatial objects. Stereo is decomposed into a 12-object 7.1.4 bed rendered through `ISpatialAudioClient`; 5.1/7.1 sources are promoted to positioned objects at their reference locations.

## How it works

Stereo input first runs through a streaming STFT that extracts the phantom centre **per frequency bin** (Avendano-Jot style phase/amplitude mask), so vocals are peeled cleanly away from overlapping instruments instead of the whole low-mid band being dragged along. The delay-aligned L/R residual is then split into four frequency bands and analysed for stereo correlation and transients. The results feed 12 spatial objects:

| Object | Content | Position |
|---|---|---|
| Sub-bass | Lowpassed mid | Centre, grounded |
| Vocal | Per-bin spectral centre (temporally smoothed) | Front centre (steered) |
| Left / Right | Delayed L/R with the spectral centre peeled out | Front L/R (steered) |
| Side L/R | Decorrelated band ambient, transient-ducked | ±90° (or rear, per SurroundPos) |
| Back L/R | Presence-band ambient | ±135° |
| Top-front / -back L/R | Side-signal band blends, brightness-steered elevation | Overhead front / rear |

Side and back objects also receive an early-reflection pre-delay and a light feedback-diffusion tail for a sense of depth. 5.1/7.1 input is promoted straight to positioned objects with no remixing; if spatial output is unavailable, a channel-based upmix is used instead.

## Requirements

- Windows 10/11
- [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) on your Atmos output device
- [Dolby Access](https://www.microsoft.com/store/productId/9N0866FS04W8) set to "Dolby Atmos for Home Theater"

## Installation

1. Build or download `MagicSpatial.dll`
2. Copy to e.g. `C:\Program Files\EqualizerAPO\VSTPlugins\`
3. In E-APO Configuration Editor, add a VST Plugin filter pointing to the DLL

Or in `config.txt`:
```
Channel: 1 2 3 4 5 6 7 8 9 10 11 12
VSTPlugin: Library "C:\Program Files\EqualizerAPO\VSTPlugins\MagicSpatial.dll"
```

## Parameters

| Parameter | Options | Default |
|---|---|---|
| **Mode** | Auto, Stereo, 5.1, 7.1, Passthrough | Auto |
| **Speakers** | 2.0 (Headphones), 5.1.2, 5.1.4, 7.1.2, 7.1.4 | 5.1.2 |
| **SurroundPos** | Side (90°), Rear (135°) — only for 5.1/5.1.2/5.1.4 | Rear |
| **Volume** | -12…+12 dB master output trim | 0 dB |

**Mode** controls input detection. Auto analyses channel activity and selects the appropriate path. **Speakers** is used by the channel-based fallback when spatial audio is unavailable. **Volume** applies a master gain to everything the plugin emits (spatial objects and channel fallback alike), useful when shared-mode Atmos arrives quieter than stereo. Note it only affects audio that flows through the Windows shared-mode mixer — exclusive-mode and bitstream-passthrough streams bypass all APOs and are untouched.

## Building

Requires Visual Studio 2025 (v145 toolset). No external dependencies.

```
Open MagicSpatial.sln → Release | x64 → Build → build\Release\MagicSpatial.dll
```

## Architecture

```
src/
  vst/           VstDefs.h, MagicSpatialVst.*, VstEntry.cpp
  audio/         SpatialObjectWriter.*  (ISpatialAudioClient render thread,
                 started only in the audio-engine instance)
  processing/    SpectralSeparator, MultibandSplitter, TransientDetector,
                 StereoCorrelationAnalyzer, Decorrelator, FeedbackDiffuser,
                 Biquad, EnhancedStereoUpmixer, Surround51/71Upmixer,
                 UpmixEngine, ChannelLayout
  core/          Types.h, Log.h
```

## Latency

~21 ms algorithmic (1024-sample STFT at 48 kHz, 50% overlap). The spectral vocal extractor is the only lookahead stage; everything downstream uses zero-latency IIR filters. The spatial render thread adds ~10 ms asynchronously without blocking the audio pipeline.

## License

[MIT](LICENSE)
