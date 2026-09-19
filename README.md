# MagicSpatial

An [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) plugin that turns stereo audio into Dolby Atmos spatial objects. Stereo is decomposed into a 12-object 7.1.4 bed rendered through `ISpatialAudioClient`; 5.1/7.1 sources are promoted to positioned objects at their reference locations.

## How it works

Stereo input first runs through a streaming STFT that extracts the phantom centre **per frequency bin** (Avendano-Jot style phase/amplitude mask), so vocals are peeled cleanly away from overlapping instruments instead of the whole low-mid band being dragged along. The delay-aligned L/R residual is then split into four frequency bands and analysed for stereo correlation and transients. The results feed 12 spatial objects:

| Object | Content | Position |
|---|---|---|
| Sub-bass | Mid below LfeCut (LR4) | LFE bed channel (direct to sub); dynamic object at front centre if the endpoint has no bed |
| Vocal | Per-bin spectral centre (temporally smoothed) | Front centre (steered) |
| Left / Right | Delayed L/R with the spectral centre peeled out | Front L/R (steered) |
| Side L/R | Decorrelated band ambient, transient-ducked (transients sensed above 200 Hz, so a kick does not pump the wash), plus low-mid body and transient snap | ±90° (or rear, per SurroundPos) |
| Back L/R | Presence-band ambient, plus low-mid body and transient snap | ±135° |
| Top-front / -back L/R | Side-signal band blends, plus low-mid body and transient snap at half weight | Static height bed channels (direct to the height speakers); brightness-steered dynamic objects if the endpoint has no height beds |

Every surround and height pair is fed its blend in antiphase, and a symmetric pair doing that cancels at the seat wherever the head does not shadow the ears, which is everything below roughly 1.5 kHz. Each pair therefore runs its two copies through allpass chains held ~180° apart from below the pair's crossover upward, so the copies add at the seat across the band instead of nulling the whole mid range. Because bass and low-mids are mono in nearly every mix, the side signal carries nothing below ~300 Hz; a separate feed of the mid signal below 350 Hz is decorrelated with short delays and low-break allpasses and added in phase to the surrounds and heights, so the envelopment has body rather than presence alone; it has no highpass of its own, so each speaker's own wash corner sets its bottom at full strength, and it joins after the bass redirect so none of its copies reach the sub. Above 350 Hz the wash is side signal only, pre-delayed, diffused and transient-ducked so the surrounds do not echo every hit, which also keeps the room out of the beat; a second mid feed covers 350 Hz to 4 kHz and is keyed by the square of the transient envelope, so only the first few milliseconds of each hit pass, decorrelated in-band and added in phase with the fronts, giving the hit reinforcement from all around without the sustained content that reads as mud. The surround and height objects are bass-managed too, at fixed corners that track those speakers' own crossovers rather than the fronts' LfeCut (120 Hz for the surrounds, 100 Hz for the heights): each is highpassed there and the summed remainder joins the LFE bed, so energy those smaller drivers could never voice arrives from the sub instead of being spent as excursion and returned as intermodulation harshness. Antiphase side content cancels in that sum, which is faithful - a symmetric pair cancels it at the seat at these wavelengths anyway - and the in-phase low-mid body is kept out of it, since the sub already carries the mid's bass. The whole upmix runs to an energy budget: the stereo bypass power is divided between the front pair (75%) and the surround/height wash (25%), with the wash measured each block and scaled to fit, so eight speakers put no more into the room than two would have. That budget is split band by band as well as in total. A broadband quota is drawn from the whole spectrum, which in music is dominated by the bass, yet the wash spends it wherever the side signal lives, and in most mixes that is the presence band, since the low end is mono and lead vocals are centred while reverb and wide synths are not. Measured on real programme, a broadband quota alone put about +2.7 dB at 4 kHz into the room against a stereo bypass while the extremes sat near +1, which is heard as the image brightening; weighing each band against its own bypass energy instead brings that peak down to +0.7 dB. The correction is applied where the side bands are formed and renormalised to leave the total alone, so it changes the wash's colour rather than its level. Because the wash is built from the side signal and so carries almost no bass, an unbudgeted upmix tilts the room bright and leaves the sub outgunned; the sub sits outside the budget and holds its level while everything around it is reined in. Side and back objects also receive an early-reflection pre-delay and a light feedback-diffusion tail for a sense of depth. 5.1/7.1 input is promoted straight to positioned objects with no remixing, apart from bass management, which uses two corners: the front three hand over at LfeCut while the surrounds and heights hand over at the same 120 Hz and 100 Hz the stereo path's wash uses, since those are smaller drivers crossed higher. Each channel's remainder joins the source's own LFE on the bed, so a game's bass from any direction reaches the sub. SubLevel scales that redirected bass but never the source's LFE channel, whose level a film's mixer set deliberately; if spatial output is unavailable, a channel-based upmix is used instead.

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
| **LfeCut** | 40 / 65 / 80 / 100 / 120 Hz sub-bass crossover | 80 Hz |
| **SubLevel** | -12 / -9 / -6 / -3 / Match / +3 / +6 dB of sub weight vs bypass | Match |
| **BassMgmt** | Plugin, Receiver | Receiver |

**LfeCut** sets the crossover between the speakers and the LFE bed: the fronts (stereo) or every channel (5.1/7.1) are highpassed here and the complementary lowpass goes to the sub, so every frequency is reproduced exactly once and the acoustic sum is flat. Set it to your front speakers' own low-frequency limit so the sub takes over precisely where they stop. **SubLevel** then sets the sub's weight, and its readings are honest: bass is mono in nearly every mix, so bypassed, both fronts radiate it coherently and the seat hears their sum; Match reproduces exactly that, and the steps run either side. Note that the bed is fed at unity. The +10 dB an LFE channel receives in Dolby Digital, DTS and their kin is applied by the decoder, and an ISpatialAudioClient bed takes linear PCM with no decode step, so what the plugin writes is what the sub plays. **Mode** controls input detection. Auto analyses channel activity and selects the appropriate path. **Speakers** is used by the channel-based fallback when spatial audio is unavailable. **Volume** applies a master gain to everything the plugin emits (spatial objects and channel fallback alike), useful when shared-mode Atmos arrives quieter than stereo. Note it only affects audio that flows through the Windows shared-mode mixer — exclusive-mode and bitstream-passthrough streams bypass all APOs and are untouched. **BassMgmt** says who owns the crossovers. An AV receiver that renders the Atmos stream applies its own speaker-size bass management to the feeds it renders, after the plugin has already crossed the objects over; the plugin's LR4 stacked on the receiver's own highpass (a second-order Butterworth on a Small speaker, the THX recipe of 12 dB/oct up and 24 dB/oct down to the sub) leaves that speaker 9 dB down at the corner instead of 6, and the receiver's redirect finds little left for the sub, so the sum has a hole of a few dB around every corner. Set Plugin if the receiver's speakers are all Large (the plugin does the whole job and the corners above apply), or Receiver if they are Small with the receiver's own crossovers, in which case every object leaves full range, nothing is redirected and the bed carries only a multichannel source's own LFE. LfeCut and the SubLevel steps below Match are then inert, but the steps above Match still work, as a lift: the bed carries the mid's bass below LfeCut scaled by the step's surplus over unity, so +3 dB adds to whatever the receiver redirects rather than replacing it, and no corner is crossed twice; set LfeCut to the receiver's own front corner so the lift lands under it. The low-mid body feed is the one exception in Receiver mode: it is a copy of the mid on six speakers, and the receiver would redirect all six copies' bass onto the sub on top of the fronts' own, so that feed alone still takes its object's corner inside the plugin, with a single second-order stage that completes the receiver's own into one LR4 rather than a second one stacked on it.

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
