# MagicSpatial

A VST2 plugin for [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) that upmixes stereo audio to Dolby Atmos using true spatial audio objects. For stereo sources, it decomposes the audio into positioned 3D objects via the Windows `ISpatialAudioClient` API, giving Dolby Access real object-based content to render. For 5.1 and 7.1 sources, it passes them through cleanly so Dolby Access can apply its own height synthesis.

## How it works

### Stereo sources (music, games, voice calls)

MagicSpatial creates **8 positioned 3D audio objects** via `ISpatialAudioClient`, each carrying a different component of the stereo signal:

| Object | Position | Content |
|---|---|---|
| **Sub-bass** | Centre, grounded | 80Hz lowpassed mid signal |
| **Vocal** | Front centre | Correlated content across 3 frequency bands (sub-bass + low-mid + high-mid) |
| **Left** | Front left | Full original left channel (source-preserving) |
| **Right** | Front right | Full original right channel (source-preserving) |
| **Surround L** | Behind left | Decorrelated ambient: low-mid room tone + high-mid + treble side |
| **Surround R** | Behind right | Decorrelated ambient: low-mid room tone + high-mid + treble side |
| **Height L** | Overhead left | Ambient extraction: diffuse content with HRTF high-shelf emphasis |
| **Height R** | Overhead right | Ambient extraction: diffuse content with HRTF high-shelf emphasis |

The decomposition uses:

- **4-band frequency splitting** (Linkwitz-Riley crossovers at 200Hz, 2kHz, 8kHz) to route each frequency range to appropriate spatial positions
- **Per-band stereo correlation analysis** to separate centre-panned content (vocals, dialogue) from ambient/decorrelated content (reverb, room tone)
- **Transient detection** (dual envelope follower) to keep impacts focused in front objects and duck surround/height objects during transients
- **Dynamic position steering** that subtly shifts L/R and vocal object positions based on the stereo energy balance, creating a living spatial image
- **Ambient field extraction** for height objects based on the Dolby/DirAC principle: only diffuse, decorrelated content goes overhead, with treble emphasis matching natural HRTF perception of overhead sound

Dolby Access receives these positioned objects and renders them to your actual speaker layout.

### 5.1 and 7.1 sources

Passed through the channel-based path with minor enhancements (back surround synthesis for 5.1). Dolby Access applies its own height synthesis from the surround bed, which works well for multichannel content that already has intentional spatial positioning.

### Native Atmos content

Applications using the Windows Spatial Audio API (`ISpatialAudioClient`) bypass E-APO entirely. In Auto mode, MagicSpatial also detects 7.1.4 content (signal on height channels) and passes it through untouched.

## Requirements

- Windows 10/11
- [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) installed on your Atmos output device
- [Dolby Access](https://www.microsoft.com/store/productId/9N0866FS04W8) set as the Spatial Sound provider ("Dolby Atmos for Home Theater")

## Installation

1. Download or build `MagicSpatial.dll` (see [Building](#building) below)
2. Copy the DLL somewhere permanent, e.g. `C:\Program Files\EqualizerAPO\VSTPlugins\MagicSpatial.dll`
3. Open **Equalizer APO Configuration Editor**
4. Add a new filter and select **VST Plugin**
5. Browse to `MagicSpatial.dll`
6. The plugin will load and automatically activate spatial audio objects

### E-APO config.txt (manual setup)

```
Channel: 1 2 3 4 5 6 7 8 9 10 11 12
VSTPlugin: Library "C:\Program Files\EqualizerAPO\VSTPlugins\MagicSpatial.dll"
```

## Parameters

Open the plugin panel in E-APO's configurator:

| Parameter | Options | Default | Description |
|---|---|---|---|
| **Mode** | Auto, Stereo, 5.1, 7.1, Passthrough | Auto | Input format. Auto detects from channel activity. |
| **Speakers** | 2.0 (Headphones), 5.1.2, 5.1.4, 7.1.2, 7.1.4 | 5.1.2 | Your physical speaker layout (used for the channel-based fallback path). |

### How the two paths work

| Input | Processing Path | Output Method |
|---|---|---|
| **Stereo** | Multiband decomposition into 8 spatial objects | `ISpatialAudioClient` dynamic objects to Dolby Access |
| **5.1** | Channel passthrough + back surround synthesis | E-APO channels to Dolby Access (Dolby adds heights) |
| **7.1** | Channel passthrough | E-APO channels to Dolby Access (Dolby adds heights) |
| **7.1.4 / Atmos** | Passthrough | E-APO channels (untouched) |

### Atmos passthrough

In **Auto** mode, MagicSpatial detects existing Atmos content (signal present on height channels 8-11) and switches to passthrough, leaving the audio untouched. Applications using `ISpatialAudioClient` natively bypass E-APO entirely.

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
    MagicSpatialVst.h/.cpp      -- Plugin class, parameters, Win32 editor UI,
                                   spatial object decomposition
    VstEntry.cpp                -- DLL entry point (VSTPluginMain)
  audio/                        -- Spatial audio output
    SpatialObjectWriter.h/.cpp  -- ISpatialAudioClient wrapper: activates spatial
                                   audio via IMMDevice::Activate, manages dynamic
                                   objects, background render thread
  processing/                   -- DSP engine
    EnhancedStereoUpmixer.*     -- Multiband spatial stereo upmixer (channel-based fallback)
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
    Log.h                       -- File logger (%APPDATA%\MagicSpatial\spatial.log)
```

### Key technical details

- **Spatial audio activation**: Uses `IMMDevice::Activate` for `ISpatialAudioClient` (the async `ActivateAudioInterfaceAsync` path does not work with Dolby Atmos for Home Theater)
- **Background render thread**: A dedicated MTA COM thread handles the spatial audio render loop, receiving decomposed audio from the VST audio thread via lock-free exchange buffers
- **Stream recovery**: If the spatial audio stream is temporarily unavailable (e.g. after E-APO reloads the plugin), retries up to 100 times with 200ms intervals
- **Fallback**: If spatial audio activation fails entirely, the plugin falls back to the channel-based `EnhancedStereoUpmixer` path automatically

## Latency

The plugin adds no measurable latency. All processing uses IIR biquad filters and envelope followers which produce output on the same sample as input. The only delay is from the decorrelator pre-delays (max 37 samples = 0.77 ms at 48 kHz), which is imperceptible. The spatial audio render thread adds one buffer cycle (~10ms) but this is handled asynchronously and does not block the audio pipeline.

## Log file

Diagnostic output is written to `%APPDATA%\MagicSpatial\spatial.log`. This includes spatial audio activation status, object count, and any errors.

## License

This project is provided as-is for personal use.
