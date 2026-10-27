# MagicSpatial

An [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) plugin that turns stereo audio into Dolby Atmos spatial objects. Stereo is upmixed into a 12-object 7.1.4 bed rendered through `ISpatialAudioClient`; 5.1/7.1 sources are promoted to positioned objects at their reference locations.

Vocals go to the centre, the left/right pair stays up front, and the ambience is spread as a diffuse wash across the surrounds and heights. The upmix puts no more power into the room than plain stereo would, and bass is routed to the sub so small speakers never strain.

For the full description of how it works internally, see [DESIGN.md](DESIGN.md).

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
| **SurroundPos** | Side (90°), Rear (135°) - only for 5.1/5.1.2/5.1.4 | Rear |
| **Volume** | -12 to +12 dB master output trim | 0 dB |
| **LfeCut** | 40 / 65 / 80 / 100 / 120 Hz sub-bass crossover | 80 Hz |
| **SubLevel** | -12 / -9 / -6 / -3 / Match / +3 / +6 dB of sub weight vs bypass | Match |
| **BassMgmt** | Plugin, Receiver | Receiver |

- **Mode** selects the input path. Auto watches channel activity and picks for you.
- **Speakers** is only used by the channel-based fallback when spatial audio is unavailable.
- **SurroundPos** places the side objects at 90° or 135° on 5.1-style layouts.
- **Volume** trims everything the plugin emits. Useful when shared-mode Atmos arrives quieter than stereo. Exclusive-mode and bitstream streams bypass all APOs and are untouched.
- **LfeCut** is the crossover between the speakers and the sub. Set it to your front speakers' low-frequency limit.
- **SubLevel** sets the sub's weight. Match gives the same bass you would hear from plain stereo; the steps run either side.
- **BassMgmt** says who owns the crossovers:
  - **Plugin** - your receiver's speakers are set to Large. The plugin does all the bass management.
  - **Receiver** - your receiver's speakers are set to Small. The receiver does the bass management and LfeCut has no effect. SubLevel steps above Match add an extra sub lift.

## Building

Requires Visual Studio 2025 (v145 toolset). No external dependencies.

```
Open MagicSpatial.sln, select Release | x64, Build. Output: build\Release\MagicSpatial.dll
```

## License

[MIT](LICENSE)
