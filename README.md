# MagicSpatial

A VST2 plugin for [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) that upmixes stereo, 5.1, and 7.1 audio to Dolby Atmos in real-time. It sits in the audio pipeline before Dolby Access, enriching flat audio sources with spatial content for your Atmos speaker system or headphones.

## What it does

MagicSpatial intercepts audio on your Dolby Atmos output device and applies frequency-aware spatial processing:

- **Stereo sources** (music, games, voice calls) are analysed and expanded into a full surround bed using multiband frequency splitting, stereo correlation analysis, and transient detection
- **5.1 sources** get back surround synthesis and optional height channel generation
- **7.1 sources** get height channel synthesis
- **Native Atmos content** is detected and passed through untouched

### Enhanced spatial processing (stereo)

The stereo upmixer splits audio into four frequency bands and routes each one spatially based on its characteristics:

| Frequency Band | Spatial Treatment |
|---|---|
| Sub-bass (< 200 Hz) | Anchored to centre + LFE |
| Low-mid (200 Hz - 2 kHz) | Front stereo, vocals steered to centre via correlation analysis |
| High-mid (2 - 8 kHz) | Front with transient emphasis, ambient content spread to surrounds |
| Treble (> 8 kHz) | Distributed to sides and height channels for elevation |

**Transient detection** keeps impacts (gunshots, drum hits, footsteps) focused in the front channels while ducking surrounds, preserving clarity and directionality. This makes it suitable for gaming as well as music.

**Stereo correlation analysis** detects how similar the left and right channels are in each frequency band. Correlated (centre-panned) content anchors to the front; decorrelated (ambient) content spreads to surrounds and heights.

## Requirements

- Windows 10/11
- [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) installed on your Atmos output device
- [Dolby Access](https://www.microsoft.com/store/productId/9N0866FS04W8) set as the Spatial Sound provider on the same device (for speaker setups), or Windows Sonic / Dolby Atmos for Headphones (for headphone setups)

## Installation

1. Download or build `MagicSpatial.dll` (see [Building](#building) below)
2. Copy the DLL somewhere permanent, e.g. `C:\Program Files\EqualizerAPO\VSTPlugins\MagicSpatial.dll`
3. Open **Equalizer APO Configuration Editor**
4. Add a new filter and select **VST Plugin**
5. Browse to `MagicSpatial.dll`
6. The plugin will load with default settings (Auto mode, 5.1.2 speakers)

### E-APO config.txt (manual setup)

```
Channel: 1 2 3 4 5 6 7 8 9 10 11 12
VSTPlugin: Library "C:\Program Files\EqualizerAPO\VSTPlugins\MagicSpatial.dll"
```

## Parameters

Open the plugin panel in E-APO's configurator to access these controls:

| Parameter | Options | Default | Description |
|---|---|---|---|
| **Mode** | Auto, Stereo, 5.1, 7.1, Passthrough | Auto | Input format. Auto detects from channel activity. |
| **Speakers** | 2.0 (Headphones), 5.1.2, 5.1.4, 7.1.2, 7.1.4 | 5.1.2 | Your physical speaker layout. Channels without speakers are zeroed. |
| **Height** | 0 - 100% | 0% | Height channel synthesis level. At 0%, Dolby Access generates its own heights. |
| **Surround** | 0 - 100% | 70% | Surround synthesis intensity for the stereo upmixer. |

### Speaker layout details

- **2.0 (Headphones)** -- All spatial channels are folded down into left/right with appropriate mixing. The enhanced spatial processing still enriches the stereo image.
- **5.1.2** -- Front L/R, Centre, LFE, Surround L/R, 2 height speakers. Back surrounds and rear heights are zeroed.
- **5.1.4** -- As above but with 4 height speakers. Back surrounds still zeroed.
- **7.1.2** -- Full 7.1 bed with 2 height speakers. Rear heights zeroed.
- **7.1.4** -- All 12 channels active.

### Height gain and Dolby Access

When **Height** is set to 0% (default), the height output channels are silent. Dolby Access will then apply its own height virtualisation, which is specifically tuned for Atmos rendering. If you prefer MagicSpatial's height synthesis instead, increase the Height parameter -- Dolby Access should detect the populated height channels and pass them through.

### Atmos passthrough

In **Auto** mode, MagicSpatial detects existing Atmos content (signal present on height channels 8-11) and switches to passthrough, leaving the audio untouched. Applications that use the Windows Spatial Audio API (`ISpatialAudioClient`) bypass E-APO entirely, so native Atmos games and apps are never affected regardless of settings.

## Building

Requires Visual Studio 2025 (v145 toolset) or later.

1. Open `MagicSpatial.sln` in Visual Studio
2. Select **Release | x64**
3. Build the solution
4. The DLL is output to `build\Release\MagicSpatial.dll`

No external dependencies -- the plugin uses only the Windows SDK and a minimal set of VST2 type definitions included in the source.

## Architecture

```
src/
  vst/                          -- VST2 plugin shell
    VstDefs.h                   -- Minimal VST2 type definitions
    MagicSpatialVst.h/.cpp      -- Plugin class, parameters, Win32 editor UI
    VstEntry.cpp                -- DLL entry point (VSTPluginMain)
  processing/                   -- DSP engine (platform-independent)
    EnhancedStereoUpmixer.*     -- Multiband spatial stereo upmixer
    MultibandSplitter.*         -- LR4 crossover filter bank (4 bands)
    TransientDetector.*         -- Dual envelope follower
    StereoCorrelationAnalyzer.* -- Per-band L/R correlation
    Surround51Upmixer.*         -- 5.1 to 7.1.4 upmixer
    Surround71Upmixer.*         -- 7.1 to 7.1.4 upmixer
    UpmixEngine.*               -- Dispatcher (selects upmixer by input format)
    Biquad.*                    -- Butterworth biquad filters
    Decorrelator.*              -- Allpass cascade decorrelation
    ChannelLayout.h             -- Input format enum
  core/
    Types.h                     -- 7.1.4 channel constants
```

## Latency

The plugin adds no measurable latency. All processing uses IIR biquad filters and envelope followers which produce output on the same sample as input. The only delay is from the decorrelator pre-delays (max 37 samples = 0.77 ms at 48 kHz), which is imperceptible and identical across all modes.

## License

This project is provided as-is for personal use.
