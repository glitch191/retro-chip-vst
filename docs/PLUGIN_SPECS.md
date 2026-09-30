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
`choiceLabels` is set); continuous ones become `AudioParameterFloat`. The `group` string
is kept in a side table (`ParamInfo`) for the UI. Every parameter's host text
(`stringFromValue` / `valueFromString`) goes through the editor's formatter
(`plugin/src/ValueFormat.h`), so automation lanes and generic editors show the panel text
with its unit ("-6 dB" for an SN76489 attenuation of 3, "-18 dB" for a TL step count of 24,
"48 ms" for an SNES echo delay of 3, "50 %" for a duty choice); the host label is empty
except for the bare "x" unit. Parsing accepts that text and plain numbers in the shown unit.

Global parameters (ids fixed):

| id | type | values / range | default |
|---|---|---|---|
| `chip` | choice | NES, SNES, Genesis | NES |
| `raw_output` | bool | | off |
| `voice_mode` | choice | MIDI channel, Poly | Poly |
| `poly_channels` | int | bit mask of hardware channels usable by Poly/arp, 10 bits; 0 = chip default | 0 = chip default (NES: pulses+triangle, SNES: all, Genesis: FM 1..6) |
| `master_gain` | float dB | -24..+12 | 0 |
| `preset_gain` | float dB | -24..+36; the preset's playing-level correction, set by presets, no panel control | 0 |
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
  4. Apply the output gain, `master_gain` + `preset_gain` in dB, as one linear ramp per
     slice on the main and per-channel signals (after the chip, so the channel ratios and
     the NES mixer are unchanged).
  5. Feed the visualiser ring buffers with the main and per-channel signals.
* MIDI CC/pitch bend: pitch bend (+/- 2 semitones default, `bend_range` fixed constant)
  goes through `setChannelPitch`; sustain pedal (CC 64) holds note-offs; all other CCs go
  to `MidiLearn`. All Sound Off (120), Reset All Controllers (121) and All Notes Off (123)
  act on their own MIDI channel; All Sound Off also resets the engine at once when no
  other MIDI channel holds a note.
* Events: no MIDI event is ever dropped. A slice ends early when its note (48) or control
  (256) list is full and the remaining events start the next slice; events beyond the
  capacity at one sample offset move one sample later. Events outside the block are
  clamped into it. Events at one offset apply in MidiBuffer order (arpeggiator running:
  controls first). The pedal holds played notes, not the arpeggiator's step note-offs.
* Chip switches: a switch requested during a running crossfade waits until it ends.
* `poly_channels` 0 (the default) or a mask with no channel of the current chip selects
  the chip's default mask, resolved on the audio thread at each switch; the plugin never
  writes `poly_channels` itself.
* `activeEngine()`, `engine(ChipId)`, `activeChip()`.
* Sample loading for the user WAV import: `loadUserSample(ChipId, slot, AudioBuffer, sampleRate)`
  is message-thread only and forwards to `IChipEngine::loadSample`. The editor reaches it
  through the preset menu entry "Import sample into slot N..." (N = the chip's sample
  parameter value; disabled when the engine has no sample slots).

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
`presetName`, `presetCategory`, the MIDI learn map (a `<MidiLearn>` child holding
`<Map cc="" param=""/>` children), and `UserSamples` (chip, slot, base64 WAV).
`setStateInformation` restores all of it and re-encodes user samples through the engines
without writing any parameter: the preset is looked up on the restored chip and its
samples go into the slots the restored parameters reference; slots that held a user
sample before return to their default content (the current preset's sample for that
slot, otherwise empty through `IChipEngine::clearSample`). When the host restores off the message
thread, the preset part is queued; `getStateInformation` (any thread) then writes the
queued values.

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
manager loads the WAV into the slot referenced by the parameter's value (presets always
write that value), then calls `setSampleInfo(slot, root_note, loop_start or -1,
sample_rate)` from `assets/samples/index.json`. Before loading, factory samples in the
chip's other slots are cleared (`clearSample`); user samples are kept unless the preset
writes their slot. A failed load is logged and reported by `sampleStatus()`, which the
strip shows under the sidebar forms (hidden when empty).

Start-up samples: at construction (`loadStartupSamples()`, after `loadBanks()`), each chip's
default sample slot (the slot parameter's default) receives the sample the chip's first
factory preset in bank order puts in that slot, else the chip's first sample in index
order (currently SNES `bass_finger`, NES `bass_pluck`, Genesis `clap`), so a fresh instance
plays its SNES voices, NES DMC and Genesis DAC before any preset. They are factory samples
(not in the plugin state): applying or restoring a preset replaces them under the rules
above; with no current preset, removing a user sample from that slot brings the start-up
sample back. The SNES panel lists the loaded slots and the free APU RAM ("Samples" box).

API: `loadBanks()`, `categories(chip)`, `subcategories(chip, category)`,
`presets(chip, category, subcategory)`, `search(chip, text)` (one chip, bank order),
`searchAll(text)` (every chip, used by the editor), `apply(const Preset&)` (sets
parameters through the APVTS on the message thread, then loads samples), `current()`,
`exportCurrent(File)`, `importFile(File)`, and `next()/previous()` inside the current
filtered list (the last `presets`, `search` or `searchAll` result, so after a cross-chip
search Previous/Next may change the chip). `apply` writes the engine parameters, the
preset-managed globals (`arp_*`, `glide_*`, `poly_channels`, `preset_gain`; a missing key
takes the default) and the samples first and the `chip` last, each write in its own change
gesture; `isApplying()` is true meanwhile, so the editor can tell a preset's chip change
from the chip selector.

Search rule (`Preset::matches`, shared by `search` and `searchAll`):

* The text is split into words at white space; every word must match (AND), each one on
  any field. Matching ignores case. Empty text matches every preset.
* A word that names a chip (`nes`, `snes`, `genesis`, or `gen`, the chip label of the
  results list) matches the presets of that chip and nothing else, so "genesis bass" and
  "nes lead" work. Chip words are exact words, not substrings: every factory SNES name
  starts with "SNES", which contains "nes", so a substring rule would make "nes" return
  the SNES bank too.
* Any other word matches when it is a substring of the name, the category, the
  subcategory or one of the tags ("bass" finds the Bass and FM Bass categories and every
  name or tag containing "bass"; "pad echo" finds SNES Pad / Echo).
* `searchAll` sorts the results by chip (NES, SNES, Genesis), then category, subcategory
  and name (natural order, ignoring case). About 1042 presets are scanned per call, on the
  message thread, when the search text changes.

## Randomizer (`Randomizer.h/.cpp`)

`randomize(ChipId chip, float amount 0..1, uint32 seed)`: for every engine parameter of
the active chip shown on the panel, draw a new value uniformly inside
`[max(min, v - amount * range), min(max, v + amount * range)]` where `range =
max - min`, rounded for integers; enumerated parameters (with `choiceLabels`) are
re-drawn with probability `amount`, and so are integer 0..1 switches without labels.
Parameters never leave `[minValue, maxValue]`; the `sample`/`dac_sample` slot parameters,
`clock`, `chip_revision`, `console_filter`, `model1_lowpass` (the Genesis console output
filter, the counterpart of `console_filter`) and `main_volume`/`master_gain` are excluded.
Global parameters (performance settings, `preset_gain`) are untouched.
The UI exposes `amount` (default 0.3) and a `Randomize` button.

## MIDI learn (`MidiLearn.h/.cpp`)

* Right-click on any knob/toggle/choice -> popup "MIDI learn", "Clear MIDI mapping"
  (when mapped), showing the current CC.
* While learning, the first CC received on any channel is bound to that parameter.
  Mapping = one CC number -> one parameter id; a CC already used is reassigned.
* Incoming CC values map linearly to the parameter's normalised range on the audio
  thread through `AudioProcessorParameter::setValueNotifyingHost` deferred to the message
  thread (lock-free latest-value slot per parameter, drained by a timer or the editor's
  vblank callback; the host is notified from the message thread). Nothing is queued, so
  a dense CC stream can neither overflow nor lose the final controller position.
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
  subcategory, plus a search field over all three chips), previous/next, randomize
  amount + button, arpeggiator knobs, glide knobs, raw output toggle, voice mode, master
  gain, UI scale.
* Preset search: the field ("Search all presets") runs `searchAll` at each text change and
  shows the results in a list under it (640 px wide, right-aligned to the field, up to 16
  rows of 24 px before it scrolls). The list's header line gives the total and the count
  per chip ("193 presets match: NES 61, SNES 41, Genesis 91") or "No preset matches". Rows
  are grouped under a bold chip header ("SNES (41)", not selectable); each result row shows
  a fixed-width chip tag ("NES", "SNES", "GEN": surface fill, 1 px border, 4 px radius, body
  text), the preset name, and "Category / Subcategory" in the dim text colour; a row cut
  with an ellipsis has a tooltip with the full text. Choosing a result (click, or Up/Down
  then Return; Return alone takes the first result) applies it: a result of another chip
  switches `chip`, the panel follows, and the search text stays; the results become the
  Previous/Next list. Escape clears the search. Changing the chip with the chip selector
  clears the search. No timer is involved; the list repaints only when the text or the
  selection changes.
* `ChannelScope`: one small waveform per hardware channel of the active chip plus main,
  reading `VisualizerBuffers`; repaints only when new samples arrived.
* `DiagnosticsOverlay`: toggled from the strip (button "Diagnostics"): detected refresh
  rate (from `VBlankAttachment` timestamps), mean frame time, worst 1 % frame time over
  the last 2 s, repaint count per second. Frame time = the editor's work per frame (the
  vblank callback plus the paints since the previous vblank), not the interval between
  frames. Off by default, costs nothing when off. The overlay takes the clicks on its area.
* Rendering: `juce::VBlankAttachment` on the editor drives scope updates and any
  transition; no `juce::Timer` for painting; nothing repaints when nothing changed
  (verify with the diagnostics repaint counter at rest = 0). The text caret does not
  blink, so a focused search field is at rest too. Scopes read their rings at most at
  about 60 Hz and skip rings that only received silence.
* Keyboard: only the preset search field takes keyboard focus (Up/Down choose a result,
  Return loads it, Escape clears). While it has the focus it consumes every key it gets
  (characters, Space, Backspace, Delete, arrows, Home/End, Tab, function keys, Return,
  Escape): `keyPressed` and `keyStateChanged` return true, so JUCE reports each key as
  handled and the host does not also use it (computer-keyboard note input in Renoise and
  others). Combinations with Ctrl or Alt that the text editor does not use (host shortcuts
  such as Ctrl+S, Alt+F4) pass to the host. Escape, Return on a result and a mouse press
  anywhere else in the editor give the keyboard back: no component keeps the focus and, in
  a host window on Windows, the native focus returns to the host's parent window
  (`ui/KeyboardFocus.h`). Other controls never keep the focus, so the host keeps Space and
  the arrow keys. `EDITOR_WANTS_KEYBOARD_FOCUS` is TRUE; in JUCE 9 it only changes the macOS
  wrappers and VST2 (`effKeysRequired`): on Windows the VST3 wrapper does not implement
  `IPlugView::onKeyDown`, and keys reach the editor as window messages through JUCE's
  thread message hook, which swallows a key when the focused component reports it handled
  (with a text field focused, a key that produces a character is swallowed as soon as
  TranslateMessage produces it; the others when the field reports them handled). Limit: a
  host that reads the keyboard another way (raw input, a low-level hook, or keys sent only
  through `onKeyDown`) can still see the keys; this is checked by the plugin tests up to
  JUCE's key handling, and in Renoise by hand.
* Text that can be cut with an ellipsis (preset name, search results, scope names, group
  titles, cluster captions, combo values in the operator grid) has a tooltip with the
  full text.
* Sizing: default 1280 x 720 logical px at scale 1.0; resizable with fixed aspect ratio
  through `ui_scale` 1.0..2.0 (the editor uses `setScaleFactor` / transform); every text
  13 px or larger at scale 1.0. Checked at 2560 x 1440, 1920 x 1080 and 1707 x 960. The
  UI scale box offers 100, 150 and 200 % (125 % and 175 % put 1 px lines on fractional
  device pixels); entries and corner sizes larger than the display's work area are not
  offered. A corner drag writes `ui_scale` once, when the drag ends.
