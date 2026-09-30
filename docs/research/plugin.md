# Plugin layer: decisions on points the specifications leave open

The plugin layer implements no chip behaviour, so it has no hardware sources. This file
records the points where docs/PLUGIN_SPECS.md (or MIDI practice) is ambiguous and the
decision the plugin layer took, in the format of the chip research notes, for the merge
into docs/HARDWARE_NOTES.md.

## Sources

| Source | URL | Consulted | Used for |
|---|---|---|---|
| MIDI 1.0 Detailed Specification (MIDI Association), Channel Mode messages | https://midi.org/midi-1-0-detailed-specification | not re-read for this change (general knowledge, 2026-09-28) | CC 120, 121, 123 act on the channel they are sent on; to be checked against the document |
| JUCE 9.0.3 sources (third_party/JUCE, AGPLv3/commercial framework the plugin links) | third_party/JUCE/modules | 2026-09-28 | CaretComponent timer, PopupMenu look-and-feel lookup, keyboard focus on mouse click |

## Ambiguities

### Sustain pedal and the arpeggiator

* Point: PLUGIN_SPECS says the pedal "holds note-offs" and that events go through the
  arpeggiator first. Applied literally, the pedal holds every arpeggiator step's note-off,
  so steps pile up on all Poly channels while the pedal is down.
* Decision: the pedal holds note-offs of played notes only. While the arpeggiator runs
  (enabled, or still holding keys / pending note-offs) its output note-offs always apply;
  the arpeggiator's own Hold keeps released keys in the pattern.
* Alternative: route CC 64 into the arpeggiator as a second Hold (released keys stay in
  the pattern while the pedal is down). Not chosen: it duplicates `arp_hold` and changes
  the pattern, which the spec does not ask for.

### Order of events at one sample offset

* Point: the spec splits the block at event offsets but does not say in which order
  control events and notes at the same offset apply.
* Decision: MidiBuffer order while the arpeggiator is idle (its output is then its input).
  With the arpeggiator running its output only carries offsets, so controls apply before
  notes at the same offset.
* Alternative: controls always first (the previous behaviour), which turns "note-off then
  pedal down" into a held note.

### Event capacity

* Point: the engine event lists have fixed capacities (64 notes in chipdsp, 256 controls).
* Decision: a slice ends early when a list is full and the remaining events start the next
  slice. Events beyond the capacity at a single sample offset therefore move one sample
  later (at most one sample per 48 notes or 256 controls at that offset). Nothing is
  dropped. The note input is limited to 48 per slice so the arpeggiator has room for its
  own steps and note-offs.
* Alternative: coalescing pitch bend / CC values; not needed once slices can split.

### Channel Mode messages

* Point: the spec does not say how CC 120/121/123 behave in MIDI channel mode.
* Decision: each acts on its own MIDI channel. All Sound Off (120) releases that channel's
  notes and, when no other MIDI channel holds a note (key or pedal), resets the engine so
  release tails stop at once; the engines have no per-channel hard cut, so otherwise the
  channel's notes release normally. The arpeggiator, which collects keys of every channel,
  restarts on any channel's CC 120 / 123.
* Alternative: global effect on every channel (the previous behaviour).

### Chip default of poly_channels

* Point: `poly_channels` has a "chip default", but one host parameter cannot hold three
  defaults, and rewriting it from the plugin when the chip changes records automation the
  user never touched and lags the audio thread.
* Decision: the parameter's default is 0, meaning "the chip's default mask" (NES pulses +
  triangle, SNES all voices, Genesis FM 1..6), resolved on the audio thread at every chip
  switch. A mask with no channel of the current chip also falls back to the chip default.
  Any other mask is kept across chips.
* Alternative: one mask parameter per chip (three host parameters instead of one).

### Second chip switch during a crossfade

* Point: the spec asks for a 20 ms equal-power crossfade per switch; it does not cover a
  switch requested while a fade runs.
* Decision: the new switch waits until the running fade has ended (checked at the start of
  every slice, so at most 20 ms plus one block). No engine is cut.
* Alternative: restarting the fade from the current gains, which would make three engines
  audible at once.

### Diagnostics "frame time"

* Point: "mean frame time" can mean the interval between frames (always 1 / refresh rate
  unless frames are skipped) or the time the editor spends per frame.
* Decision: the time spent per frame: the vblank callback plus every editor paint since
  the previous vblank. The interval is already shown as the refresh rate.

### Blinking text caret

* Point: the rule "nothing repaints at rest" conflicts with JUCE's blinking caret, which
  runs its own timer while a text field has focus.
* Decision: the editor's look and feel creates a caret that does not blink; it shows while
  the field has focus.

### Per-preset playing level (preset_gain)

* Point: single voices played at about -30 dBFS RMS (Genesis PSG about -45) because the mix
  is hardware-relative, and the product owner asked (2026-09-30) for one common playing
  level per preset without touching the chip mix. Open: where the gain acts, and how a
  preset's level is measured when the note decays during the hold.
* Decision: a global `preset_gain` (-24..+36 dB, default 0), preset-managed like `arp_*`,
  `glide_*` and `poly_channels` (a preset without it resets it to 0 dB), not on the panel,
  never randomized. EngineHost adds it to `master_gain` in dB and applies the sum after the
  chip output in the existing gain ramp, on the main and channel buses, so channel ratios
  and the NES non-linear mixer are unchanged. The generator's measure is the stereo RMS of
  the 10 ms windows of the held C4 within 20 dB of the loudest one, targeted at -18 dBFS,
  with the sample peak kept at or below -1 dBFS (docs/PRESET_SPECS.md, "Playing level").
* Alternative: RMS over the whole hold (percussive sounds would all end at the peak
  ceiling), a loudness model (ITU-R BS.1770 K-weighting), or a per-chip gain.
