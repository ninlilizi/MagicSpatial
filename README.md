# MagicSpatial

An [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) plugin that turns stereo audio into Dolby Atmos spatial objects. Stereo sources are decomposed into 8 positioned 3D objects rendered through `ISpatialAudioClient`; multichannel sources pass through for Dolby Access to handle natively.

## How it works

Stereo input is split into four frequency bands and analysed for stereo correlation (centre vs ambient) and transients (impacts vs sustain). The results feed 8 spatial objects:

| Object | Content | Position |
|---|---|---|
| Sub-bass | Lowpassed mid | Centre, grounded |
| Vocal | Correlated content across 3 bands, transient-boosted | Front centre (dynamically steered) |
| Left / Right | Full original L/R | Front left/right (dynamically steered) |
| Surround L/R | Decorrelated ambient from low-mid, high-mid, treble | Behind, transient-ducked |
| Height L/R | Diffuse ambient with HRTF high-shelf emphasis | Overhead |

5.1/7.1 input passes through the channel-based path so Dolby Access applies its own height synthesis. Native Atmos content (via `ISpatialAudioClient` or detected 7.1.4) is never modified.

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

**Mode** controls input detection. Auto analyses channel activity and selects the appropriate path. **Speakers** is used by the channel-based fallback when spatial audio is unavailable.

## Building

Requires Visual Studio 2025 (v145 toolset). No external dependencies.

```
Open MagicSpatial.sln → Release | x64 → Build → build\Release\MagicSpatial.dll
```

## Architecture

```
src/
  vst/           VstDefs.h, MagicSpatialVst.*, VstEntry.cpp
  audio/         SpatialObjectWriter.*  (ISpatialAudioClient, render thread)
  processing/    MultibandSplitter, TransientDetector, StereoCorrelationAnalyzer,
                 Decorrelator, Biquad, EnhancedStereoUpmixer, Surround51/71Upmixer,
                 UpmixEngine, ChannelLayout
  core/          Types.h, Log.h
```

## Latency

Sub-millisecond. All DSP uses zero-latency IIR filters. The spatial render thread adds ~10ms asynchronously without blocking the audio pipeline.

## License

[MIT](LICENSE)
