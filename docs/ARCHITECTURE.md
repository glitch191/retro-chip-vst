# Architecture

This document is the contract every module follows. Hardware details live in
`docs/research/*.md` (tables, formulas, sources) and the decisions taken where the
documentation is ambiguous live in `docs/HARDWARE_NOTES.md`.

## Layers

```
plugin/   JUCE layer (VST3): PluginProcessor, parameters, editor, presets, MIDI learn
   |
   v      IChipEngine (dsp/include/chipdsp/IChipEngine.h)
dsp/      chipdsp static library, no JUCE: engines, arpeggiator, glide, resampling
tools/    Python generators: preset banks, samples (procedural and converted CC0 recordings), QA reports
assets/   Generated preset banks (JSON) and samples (WAV), embedded in the binary
```

`chipdsp` compiles and tests without JUCE (`cmake --preset dsp-only-release`).

## Engine contract (`IChipEngine`)

* One engine per chip: `Nes2A03Engine`, `SnesDspEngine`, `GenesisEngine`.
* `channel` always indexes hardware channels (NES 0..4, SNES 0..7, Genesis 0..5 FM then 6..9 PSG).
* Pitch is passed as a float MIDI note. The engine converts to its own register
  resolution and never smooths the result: the 11-bit period steps of the 2A03,
  the block/fnum grid of the YM2612 and the 14-bit pitch of the S-DSP are audible on
  purpose.
* Parameters are addressed by an engine-specific integer id and carry native units
  (a duty index 0..3, a total level 0..127, an echo delay 0..15). Each engine publishes
  `parameterDescriptors()`; the plugin builds its host parameters from that list and
  the randomizer draws inside `[minValue, maxValue]`, so it cannot leave hardware bounds.
* `renderBlock()` receives blocks already split at MIDI event boundaries by the plugin
  layer. Events therefore land on sample boundaries; the engine applies them at the
  start of the next block.
* Sample slots (`loadSample`, `setSampleInfo`, `clearSample`) are message-thread only.
  Engines keep two pre-allocated banks (three on the NES) and flip an atomic index; the
  audio thread reads the active bank at block start. `clearSample` empties one slot
  through the same flip, so its memory stops counting against the hardware budget.
* The encoders depend on parameters (NES `dmc_rate`/`clock`, Genesis `dac_rate`, SNES
  `echo_delay` budget) that the audio thread forwards only at its next block. Before a
  load the plugin hands the current values to `stageParameter()` (message thread): it
  stores the parameter atomic without touching the running chip; the audio thread still
  delivers the same value through `setParameter()`.
* Plugin sample-slot policy (PresetManager): a preset's sample goes into the slot its
  slot parameter value names; every other slot of that chip holding a factory sample is
  cleared first (user-imported samples stay unless the preset writes their slot), so any
  sequence of presets stays within the SNES APU RAM budget. Root note, loop start and
  source rate from `assets/samples/index.json` go to `setSampleInfo()` after each load.
  Failed loads are logged and shown in the editor (`PresetManager::sampleStatus()`).

## Timing and resampling

Each engine steps its chip at the native clock and hands level changes to
`BandLimitedStepSynth` (`dsp/include/chipdsp/util/BandLimitedStepSynth.h`).

Algorithm: band-limited step synthesis. Every chip output is piecewise constant at
its clock (the 2A03 mixer changes at 1.79 MHz, the S-DSP DAC holds each 32 kHz sample,
the YM2612 DAC holds each 53.267 kHz sample, the SN76489 changes at 223.7 kHz). A
piecewise-constant signal is exactly a sum of steps, so resampling it is exactly a sum
of band-limited steps. For each change the synth adds a 32-tap kernel (windowed sinc,
Kaiser beta 7, 64 fractional phases) scaled by the change into an accumulation buffer;
the running sum of that buffer yields the band-limited output. Two kernels exist:

* `IntegratedStep` (Genesis, SNES): each tap is the first difference of the integrated
  windowed sinc, so the running sum gives exact samples of a band-limited step and the
  pass band is flat.
* `ImpulseSum` (NES): taps are samples of the windowed sinc itself. The discrete running
  sum then adds a gain of (w/2)/sin(w/2), w = 2 pi f / host rate (+0.94 dB at 11.2 kHz at
  44.1 kHz). Kept for the NES only because its output must not change (product-owner
  decision 2026-09-29; `docs/research/refcheck-report.md` finding F2).

Why this and not a polyphase FIR over the native stream: the NES would need 1.79 M
samples per second to be filtered per channel. The step approach costs one 32-tap
add per level change, which is proportional to the signal's activity, not the clock.

Cutoff:

* Downsampling (NES, Genesis): 0.45 cycles per host sample (90 % of Nyquist).
* Upsampling (SNES at 32 kHz into 44.1/48 kHz): cutoff at the native Nyquist
  (16 kHz), which removes the zero-order-hold images the way the console's analog
  reconstruction filter does, and keeps the hold's own high-frequency droop.

`Raw output` (global parameter) switches every synth to a zero-order hold sampled at
host instants: no anti-aliasing at all, so the hardware's aliasing folds into the
audible band, which is what a console feeding a wide-band capture chain does.

Every engine follows the resampler with a DC-blocking one-pole high-pass modelling
the output coupling capacitor, then optional console output filters (NES: 90 Hz and
440 Hz high-pass, 14 kHz low-pass per NESdev; Genesis Model 1 low-pass). These
stages are the only non-chip processing and are documented as such.

## Real-time rules

* Audio thread: no allocation, no locks, no I/O, no exceptions, no `std::function`
  allocation. Tables are built in `prepare()` or at static init.
* Parameters cross threads as atomics (`AudioProcessorValueTreeState` in the plugin,
  plain `std::atomic<float>` inside engines where needed).
* Visualisation reads single-producer/single-consumer ring buffers written by the
  audio thread; the UI never blocks the audio thread.

## Plugin layer

* One active engine, selected by the `chip` parameter. All three engines are prepared
  at `prepareToPlay` so switching never allocates. A switch runs the outgoing engine
  and the incoming engine together for 20 ms with an equal-power crossfade.
* MIDI routing: `Voice mode` = `MIDI channel` (MIDI channel N drives hardware channel
  N-1: NES 1..5, SNES 1..8, Genesis 1..6 FM and 7..10 PSG) or `Poly` (any channel;
  the voice allocator distributes notes over the enabled hardware channels with
  oldest-note stealing). The arpeggiator sits in front of the allocator.
* Buses: `Main` stereo plus one stereo pair per hardware channel (NES 5, SNES 8,
  Genesis 10). Extra buses are disabled by default; the maximum layout is the
  Genesis one and is exposed for every chip so the layout never changes at runtime.
* State: APVTS tree + current preset name + MIDI learn map + user sample data (base64
  WAV) in one `ValueTree`, saved by `getStateInformation`.
* Preset format: JSON documents with `name`, `chip`, `category`, `subcategory`,
  `tags`, `params` (key -> native value) and optional `samples`. Banks are JSON arrays
  embedded via `juce_add_binary_data`.

## Editor

* One panel per chip, swapped by the chip selector, plus a common strip
  (preset browser with search, randomize, arpeggiator, glide, raw output, MIDI learn).
* Rendering is driven by `juce::VBlankAttachment`; all motion uses elapsed time.
  Nothing repaints while idle. A diagnostics overlay reports detected refresh rate,
  mean frame time and worst 1 %.
* Style constants (colours, font, spacing) live in `plugin/src/ui/Theme.h` only.
