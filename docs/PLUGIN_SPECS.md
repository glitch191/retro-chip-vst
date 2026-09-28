# Plugin layer specification (JUCE)

Companion to `ARCHITECTURE.md` and `ENGINE_SPECS.md`. Fixes the public surface of the
plugin layer so that the processor, the editor, the preset tools and the tests agree.
Namespace for plugin code: `rcv`. Files live in `plugin/src` (core) and
`plugin/src/ui` (editor).

## Parameters (`Parameters.h/.cpp`)

All host parameters live in one `juce::AudioProcessorValueTreeState` (APVTS).

Engine parameters are generated from `IChipEngine::parameterDescriptors()` for each of
the three engines at construction. Host parameter id = `<chipKey>_<desc.key>`
(`nes_p1_duty`, `snes_echo_delay`, `genesis_op1_tl`). Name = `<ChipName> <desc.name>`.
Integer descriptors become `AudioParameterInt` (or `AudioParameterChoice` when
`choiceLabels` is set); continuous ones become `AudioParameterFloat` with the unit as
label. The `group` string is kept in a side table (`ParamInfo`) for the UI.

Global parameters (ids fixed):

| id | type | values / range | default |
|---|---|---|---|
| `chip` | choice | NES, SNES, Genesis | NES |
| `raw_output` | bool | | off |
| `voice_mode` | choice | MIDI channel, Poly | Poly |
| `poly_channels` | int | bit mask of hardware channels usable by Poly/arp, 10 bits | chip default (NES: pulses+triangle, SNES: all, Genesis: FM 1..6) |
| `master_gain` | float dB | -24..+12 | 0 |
| `arp_enabled` | bool | | off |
| `arp_pattern` | choice | Up, Down, Up-Down, As played, Random | Up |
| `arp_octaves` | int | 1..4 | 1 |
| `arp_rate_mode` | choice | Sync, Free | Sync |
| `arp_sync_division` | choice | 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 | 1/16 |
| `arp_free_rate` | float Hz | 0.5..50 | 8 |
| `arp_gate` | float % | 5..100 | 50 |
| `arp_hold` | bool | | off |
| `glide_time` | float ms | 0..2000 | 0 |
| `glide_mode` | choice | Always, Legato only | Always |
| `ui_scale` | float | 1.0..2.0 (not automatable, saved with state) | 1.0 |

`ParamInfo` gives, for every host parameter: id, display name, group, chip (or none for
globals), engine parameter id, `ParamDesc` copy, and whether it is shown on the panel.

## Engine host (`EngineHost.h/.cpp`)

Owns the three engines (all prepared), the `VoiceAllocator`, `Arpeggiator` and `Glide`,
and the crossfade.

* `prepare(sampleRate, maxBlock)`, `reset()`.
* `process(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, const chipdsp::TransportInfo&, const BusMap&)`:
  1. Read the `chip` parameter; if it changed, start a 20 ms equal-power crossfade from
     the running engine to the new one (both render during the fade; the outgoing
     engine gets noteOff on every channel at the switch, the incoming one starts silent).
  2. Push parameters to the active engine(s): each engine parameter with a changed value
     is forwarded with `setParameter` (compare against a cached value; do not forward
     unchanged values every block).
  3. Split the block at MIDI event sample offsets. For each sub-block: route events
     (arp first, then allocator/MIDI-channel mode, then glide), call the engine(s),
     accumulate into the main bus, and into per-channel buses when the host enabled them.
  4. Feed the visualiser ring buffers with the main and per-channel signals.
* MIDI CC/pitch bend: pitch bend (+/- 2 semitones default, `bend_range` fixed constant)
  goes through `setChannelPitch`; sustain pedal (CC 64) holds note-offs; all other CCs go
  to `MidiLearn`.
* `activeEngine()`, `engine(ChipId)`, `activeChip()`.
* Sample loading for the user WAV import: `loadUserSample(ChipId, slot, AudioBuffer, sampleRate)`
  is message-thread only and forwards to `IChipEngine::loadSample`.

## Bus layout (`PluginProcessor`)

`BusesProperties`: `Main` stereo (enabled) + 10 stereo buses named after the largest
chip: `Out 1` .. `Out 10`, disabled by default. Bus names carry the generic index; the
editor and README document the mapping per chip (NES: 1 Pulse 1, 2 Pulse 2, 3 Triangle,
4 Noise, 5 DMC; SNES: 1..8 voices; Genesis: 1..6 FM, 7..9 PSG tone, 10 PSG noise).
`isBusesLayoutSupported`: main must be stereo; each extra bus is stereo or disabled.
Renoise note: Renoise exposes plugin multi-outs as routing targets when the bus is
enabled; the README records what was observed.

## State

`getStateInformation` writes a `ValueTree` "RetroChipState" containing the APVTS tree,
`presetName`, `presetCategory`, the MIDI learn map (`<Map cc="" param=""/>` children),
and `UserSamples` (chip, slot, base64 WAV). `setStateInformation` restores all of it and
re-encodes user samples through the engines.

## Presets (`PresetManager.h/.cpp`)

JSON preset document:

```json
{
  "name": "NES Lead Duty25 Vibrato Slow",
  "chip": "nes",
  "category": "Lead",
  "subcategory": "Pulse",
  "tags": ["duty25", "vibrato"],
  "params": { "p1_duty": 1, "p1_volume": 12, "...": 0 },
  "global": { "arp_enabled": 0, "glide_time": 0 },
  "samples": { "dmc_sample": "kick_short" }
}
```

Banks: `assets/presets/<chip>.json` = JSON array of preset documents, embedded with
`juce_add_binary_data`. `params` keys are engine keys without the chip prefix; unknown
keys are ignored with a log line; missing keys keep the engine default. `samples` maps a
sample-slot parameter to a sample name from `assets/samples/<chip>/<name>.wav`; the
manager loads the WAV into the slot referenced by the parameter's value.

API: `loadBanks()`, `categories(chip)`, `subcategories(chip, category)`,
`presets(chip, category, subcategory)`, `search(chip, text)` (case-insensitive
substring over name and tags), `apply(const Preset&)` (sets parameters through the
APVTS on the message thread, then loads samples), `current()`, `exportCurrent(File)`,
`importFile(File)`, and `next()/previous()` inside the current filtered list.

## Randomizer (`Randomizer.h/.cpp`)

`randomize(ChipId chip, float amount 0..1, uint32 seed)`: for every engine parameter of
the active chip shown on the panel, draw a new value uniformly inside
`[max(min, v - amount * range), min(max, v + amount * range)]` where `range =
max - min`, rounded for integers; enumerated parameters (with `choiceLabels`) are
re-drawn with probability `amount`. Parameters never leave `[minValue, maxValue]`; the
`sample`/`dac_sample` slot parameters, `clock`, `chip_revision`, `console_filter` and
`main_volume`/`master_gain` are excluded. Global performance parameters are untouched.
The UI exposes `amount` (default 0.3) and a `Randomize` button.

## MIDI learn (`MidiLearn.h/.cpp`)

* Right-click on any knob/toggle/choice -> popup "MIDI learn", "Clear MIDI mapping"
  (when mapped), showing the current CC.
* While learning, the first CC received on any channel is bound to that parameter.
  Mapping = one CC number -> one parameter id; a CC already used is reassigned.
* Incoming CC values map linearly to the parameter's normalised range on the audio
  thread through `AudioProcessorParameter::setValueNotifyingHost` deferred to the message
  thread (use a lock-free FIFO of (paramIndex, value) drained by a timer or the
  editor's vblank callback; the host is notified from the message thread).
* Map is saved in the state (see above).

## Visualiser buffers (`VisualizerBuffers.h`)

Single-producer/single-consumer ring buffers, one per hardware channel plus main
(11 x 2), 8192 floats each, written by the audio thread with `std::atomic<int>`
write indices, read by the UI. No locks.

## Editor (`ui/`)

* `Theme.h`: every colour, font size, spacing and radius constant (neutral palette, one
  accent, WCAG AA contrast, 4/8 px grid, 1 px borders, radius 4 px, no gradients or
  shadows). Font: the system sans-serif (`juce::Font::getDefaultSansSerifFontName()`).
* `RcvLookAndFeel`: rotary knob with visible label and value, toggle, combo, text button,
  popup menu; 100-150 ms transitions only for control states, driven by elapsed time.
* `Knob`: rotary slider + label + value readout, attached to an APVTS parameter,
  right-click MIDI learn menu, minimum 24 px interactive target (documented density
  constraint), tooltip with full name when the label is truncated.
* `ChipPanel` base + `NesPanel`, `SnesPanel`, `GenesisPanel`: groups of knobs laid out
  from `ParamInfo` groups in the order listed in ENGINE_SPECS.md; only the active chip's
  panel is visible. No effects section for NES and Genesis.
* `CommonStrip`: chip selector, preset browser (two-level menu: category then
  subcategory, plus a search box filtering by name), previous/next, randomize amount +
  button, arpeggiator knobs, glide knobs, raw output toggle, voice mode, master gain,
  UI scale.
* `ChannelScope`: one small waveform per hardware channel of the active chip plus main,
  reading `VisualizerBuffers`; repaints only when new samples arrived.
* `DiagnosticsOverlay`: toggled from the strip (button "Diagnostics"): detected refresh
  rate (from `VBlankAttachment` timestamps), mean frame time, worst 1 % frame time over
  the last 2 s, repaint count per second. Off by default, costs nothing when off.
* Rendering: `juce::VBlankAttachment` on the editor drives scope updates and any
  transition; no `juce::Timer` for painting; nothing repaints when nothing changed
  (verify with the diagnostics repaint counter at rest = 0).
* Sizing: default 1280 x 720 logical px at scale 1.0; resizable with fixed aspect ratio
  through `ui_scale` 1.0..2.0 (the editor uses `setScaleFactor` / transform); every text
  13 px or larger at scale 1.0. Checked at 2560 x 1440, 1920 x 1080 and 1707 x 960.
