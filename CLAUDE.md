# retro-chip-vst

Hardware-faithful NES (2A03), SNES (S-DSP) and Sega Genesis (YM2612 + SN76489) synthesis
VST3 for Windows x64. C++20, CMake, JUCE 9.0.3 (plugin layer only), Catch2 v3 (tests),
Python 3 standard library only (generators and analysis tools).

## Goal

A personal instrument for music production in Renoise (target host, 3.5.x) that sounds
like the original consoles: documented hardware behaviour, limitations and quirks
included. Features: three chip engines, arpeggiator, glide, multi-outputs (one stereo bus
per chip channel), MIDI learn, randomizer bounded by hardware ranges, about 1000
generated factory presets with automatic QA, cross-chip preset search, and a sober editor
readable at 2560x1440 that repaints on the display's vblank (the owner's display runs at
360 Hz) and has a diagnostics overlay.

## Non-negotiable rules

* Everything in the repository is in English: code, comments, docs, commit messages,
  UI strings, preset names.
* Never read, copy or structurally imitate GPL emulator sources (Mesen, bsnes, Genesis
  Plus GX, Nuked-OPN2, MAME's fm.cpp, blip_buf, snes_spc, Game_Music_Emu...). Implement
  from public specifications only: NESdev wiki, Anomie's S-DSP doc, Sega manuals,
  Nemesis/Maxim/SMS Power documentation, Yamaha datasheets. Record every source in
  `docs/research/<chip>.md` (name, URL, date consulted, what was used).
* Fidelity first: reproduce documented hardware behaviour including limitations and
  quirks. Never smooth, interpolate or "improve" what the hardware quantises.
* When documentation is ambiguous or incomplete, do not guess silently: write the
  point, the sources, the decision and the alternative in `docs/research/<chip>.md`
  under "Ambiguities" (merged later into `docs/HARDWARE_NOTES.md`).
* No samples, FM patches or sequences ripped from game ROMs or soundtracks.
* `dsp/` (library `chipdsp`) must never include JUCE. It builds and tests alone.
* Audio thread (`renderBlock`, `noteOn`, `noteOff`, `setParameter`, `reset`...): no
  allocation, no locks, no I/O, no exceptions. Tables are precomputed in `prepare()` or
  as `constexpr`/static data.
* Keep the code simple and readable. No premature abstraction.
* Do not run `git` commands from subagents; the orchestrator commits.

## Decisions and their reasons

* **CC0 samples (owner decision, 2026-09-29).** SNES BRR and Genesis DAC samples come
  from CC0 recordings (VSCO 2 CE, VCSL) converted by `tools/samplegen/cc0_import.py`,
  plus procedural ones. Reason: the first synthetic samples made the SNES and Genesis
  sound far from the consoles. Sources and licences: `docs/SOURCES.md`.
* **Reference emulators as black boxes (owner decision, 2026-09-29).** VGMPlay 0.40.9
  (Nuked OPN2, MAME/GPGX, NSFPlay, MAME NES cores) and Game Music Emu through FFmpeg may
  be downloaded into `third_party/refemu/` and run to compare audio output only. Their
  code is never read and never redistributed. Details: `docs/research/reference-emulators.md`.
* **Integrated-step band-limited kernel** (`BandLimitedStepSynth`, flat pass band) for
  all engines, after refcheck finding F2. The older impulse-sum kernel is kept only for
  `chiptool regs nes --kernel impulse`.
* **YM2612 operator pipeline delays** per modulation path (refcheck finding F1): S1->S2 1,
  S1->S3 2, S1->S4 1, S2->S3 1, S2->S4 1, S3->S4 0 samples; S1 as a carrier in
  algorithm 7 is also one sample late.
* **Per-preset loudness (`preset_gain`, -24..+36 dB).** Engines stay at
  hardware-relative levels; each factory preset carries a gain computed by
  `tools/presetgen/level.py` (held-note RMS target -18 dBFS, peak ceiling -1 dBFS at
  velocity 100 and 127, rounded to 0.5 dB). Reason: raise quiet presets without changing
  the ratios between channels.
* **Rendered perceptual QA** for preset de-duplication (log-mel spectrum, 10 ms
  envelope, pitch track, through the real engines via `chiptool features`) instead of
  comparing parameters. Presets whose globals differ are never merged.
* **Genesis operator TL shown as raw steps 0..127**, not dB. A dB unit made the
  parameter text truncate in the editor.
* **Preset search**: every word must match (AND, any case) the name, category,
  subcategory or tags; the words `nes`, `snes`, `genesis`, `gen` select a chip as whole
  words only (a substring would make "nes" match every "SNES ..." name).
* **Keyboard in the search field**: while focused it consumes every key except the
  Ctrl/Alt shortcuts it does not use, and gives the focus back to the host on Escape,
  Return or a click elsewhere. `EDITOR_WANTS_KEYBOARD_FOCUS` alone has no effect on the
  Windows VST3 wrapper of JUCE 9.
* **Install target**: `C:\Program Files\Common Files\VST3` (system folder, elevated
  copy), requested by the owner.

## Layout

```
dsp/include/chipdsp/   public headers (IChipEngine.h, ChipTypes.h, util/, nes/, snes/, genesis/, perf/)
dsp/src/<area>/        implementation, one static library per area: util, nes, snes, genesis, perf, factory
dsp/tests/             Catch2 tests: test_<area>_*.cpp -> chipdsp_tests_<area>
dsp/tools/chiptool.cpp offline renderer and analysis tool (features, render, regs, dump-params)
plugin/src/            JUCE layer (namespace rcv): processor, EngineHost, parameters, PresetManager,
                       MidiLearn, Randomizer, ValueFormat, ui/ (panels, CommonStrip, diagnostics)
plugin/tests/          Catch2 plugin tests -> plugin_tests
tools/                 Python generators (gen_presets.py, gen_samples.py, presetgen/, samplegen/),
                       refcheck/, render_listening.py, run_pluginval.ps1, dev-env.ps1, tests/
assets/                generated presets (JSON) and samples (WAV), committed and embedded in the binary
docs/                  ARCHITECTURE.md, ENGINE_SPECS.md, PLUGIN_SPECS.md, PRESET_SPECS.md,
                       HARDWARE_NOTES.md, SOURCES.md, PRESET_QA.md, research/, images/
.github/workflows/     build.yml (Windows build, ctest, pluginval, artefact; Release on v* tags)
```

`docs/ARCHITECTURE.md` is the contract every module follows (engine interface, timing,
real-time rules, plugin layer, editor).

## Build and test (Windows PowerShell)

```powershell
. .\tools\dev-env.ps1                      # MSVC x64 env + bundled cmake/ninja/git
cmake --preset dsp-only-release            # chipdsp + tests, no JUCE
cmake --build --preset dsp-only-release --target chipdsp_tests_nes
ctest --preset dsp-only-release -R nes     # or run build\dsp-only-release\dsp\tests\chipdsp_tests_nes.exe
.\build.ps1                                # full build including the VST3, then ctest
.\build.ps1 -Preset windows-x64-release -Install   # also copies to Common Files\VST3 (UAC prompt)
.\tools\run_pluginval.ps1                  # pluginval strictness 5 (-Strictness 10 also exists)
python -m unittest discover -s tools/tests # Python generator tests
```

Built bundle: `build\windows-x64-release\plugin\RetroChip_artefacts\Release\VST3\Retro Chip.vst3`.
Preset and sample regeneration, the reference-emulator check and the A/B listening set
are documented step by step in `README.md`.

Subagents working in parallel must use a private build directory instead of the preset:

```powershell
. .\tools\dev-env.ps1
cmake -S . -B build\agent-<name> -G Ninja -DCMAKE_BUILD_TYPE=Release -DRCV_BUILD_PLUGIN=OFF -DRCV_BUILD_BENCH=OFF
cmake --build build\agent-<name> --target chipdsp_tests_<area>
build\agent-<name>\dsp\tests\chipdsp_tests_<area>.exe
```

Only build the targets of your own area; other areas may be mid-edit by another agent.
Delete `build\agent-*` directories when the work is merged: a plugin build directory
takes about 1.3 GB.

## Conventions

* Commits: English, imperative summary line, body when useful. Unfinished work is
  committed with a `WIP:` prefix. In PowerShell an inline multi-line `-m` breaks on
  quoting: commit from Git Bash with `git commit -F -` and a heredoc.
* Line endings: the repository uses LF. Some Windows editing tools rewrite files as
  CRLF; check `git diff --stat --ignore-cr-at-eol` against `git diff --stat` before
  committing and convert back.
* Every documentation ambiguity goes to `docs/research/<chip>.md` and
  `docs/HARDWARE_NOTES.md`; every external source goes to `docs/SOURCES.md`.
* Editor rules: no gradients, no shadows, text at least 13 px, click targets at least
  24 px, repaint only on change (driven by `VBlankAttachment`, never a fixed 60 Hz timer).

## Working with the owner

* The owner writes in French: answer in French; everything written into the repository
  stays in English.
* Give a short plan before large changes; validate with tests, measurements and
  screenshots rather than claims.
* State plainly what was not measured or tested (host behaviour in Renoise, listening);
  the owner validates in Renoise and by ear.
* Keep token use lean: reuse workflow caches, wait for task notifications instead of
  polling with sleeps.

## This machine (may not apply elsewhere)

* Nothing developer-related is on PATH in PowerShell except Python; `tools\dev-env.ps1`
  adds MSVC (VS Build Tools 2022), the cmake and ninja bundled with it, and git (from
  GitHub Desktop).
* JUCE 9.0.3 is extracted in `third_party/JUCE` (git-ignored) so build directories do
  not re-download it; without it CMake fetches the pinned archive.
* numpy and scipy are not installed; tools are stdlib-only by design.
