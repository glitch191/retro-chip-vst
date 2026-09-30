# Handoff: state of the project on 2026-09-30

Repository: `main` at `b821ee9`, in sync with `origin/main`
(https://github.com/glitch191/retro-chip-vst). Working tree clean before this file.

## Done

* Three engines behind `IChipEngine`: `Nes2A03Engine` (NesApu + NesDriver),
  `SnesDspEngine` (BRR, gaussian interpolation, echo, APU RAM budget), `GenesisEngine`
  (Ym2612Core + Sn76489Core + GenesisDriver).
* Reference-emulator check (`tools/refcheck`, report in `docs/research/refcheck-report.md`,
  last run 2026-09-30): S-DSP bit-exact with Game Music Emu apart from player gain and
  polarity; SN76489 matches; the eight YM2612 algorithms within 0.11 dB of Nuked OPN2
  after fixes F1 and F2; NES shows no engine deviation against NSFPlay.
* Plugin: arpeggiator, glide, multi-outputs, MIDI learn, randomizer, host-visible value
  text, diagnostics overlay, sample import, 20 ms crossfade on chip switch.
* Factory banks: NES 311, SNES 338, Genesis 393 (1042 in total), with `preset_gain`
  loudness and the rendered QA report in `docs/PRESET_QA.md`.
* Cross-chip preset search with chip-tagged results and the keyboard-focus fix
  (commits `1742422`, `51be099`; both are labelled WIP but the feature was finished in
  them, no change is pending).
* README with screenshots (`docs/images/`), GitHub Actions workflow
  `.github/workflows/build.yml`.
* Last local validation (2026-09-30): ctest 334/334, pluginval strictness 5 passed
  (strictness 10 last passed 2026-09-29, before the latest changes), Python generator
  tests passed at the last preset regeneration.
* Installed build: `C:\Program Files\Common Files\VST3\Retro Chip.vst3`, built from
  `51be099` (`b821ee9` only edits the README).

## In progress

Nothing uncommitted. No agent or workflow is running.

## Next steps, by priority

1. **Check the first GitHub Actions run.** The workflow was written and committed but
   has never run on GitHub as far as this session knows; its result is unknown. If
   pluginval fails only there on the GUI tests, add `-SkipGuiTests` to that step.
2. **Confirm the Renoise checks.** On 2026-09-30 the owner removed from the README the
   lines saying that Renoise (multi-out, automation, state recall, typing in the search
   field) and the 360 Hz display were not tested. Whether that means they were
   validated is not recorded: ask the owner.
3. **Decide the JUCE licence before publishing a binary** (AGPLv3 or commercial JUCE
   licence; the README says a commercial licence is planned). The workflow creates a
   public Release with the binary on any `v*` tag.
4. **First release**: tag `v0.1.0` (matches `project(VERSION 0.1.0)` in `CMakeLists.txt`).
   A proposed title and release notes were drafted in the chat session, not stored in
   the repository.
5. **Listen to the A/B set** (`python tools\render_listening.py --before-ref 61ddff8`,
   output in `build-reports\listening\`); nobody has listened to it yet.
6. Possible improvements, not started:
   * The loudness ceiling is checked per single note: chords and arpeggios can exceed
     -1 dBFS.
   * 17 Genesis PSG presets (soft level variants) sit at the +36 dB `preset_gain` bound,
     3-11 dB below the target.
   * Level variants ("Level Soft", ghost notes) now play near the common level, so they
     sound alike.
   * `poly_channels` has no editor control.

## Pitfalls met (and how they were solved)

* **Workflows and agents stop when the Claude session ends.** Resume them with
  `resumeFromRunId`; keep the machine awake during long runs.
* **First rendered QA removed about 80 % of the presets**: a cosine metric on dB vectors
  and the renderer always playing channel 0. Fixed with level-normalised shape, level,
  envelope and pitch metrics, the channel taken from `poly_channels`, sample loading,
  a pre-roll, a longer hold and a second short C3 note.
* **Genesis DC click at start-up**: fixed by syncing the level trackers plus 16 settle
  samples.
* **First envelope step lost on a frame tick** (NES and Genesis drivers): fixed with
  `deferFirstTick`.
* **Velocity-127 peaks above 0 dBFS**: the loudness ceiling now uses the velocity-127
  peak (`712a707`).
* **`render_listening.py` compared against a half-updated revision**: use
  `--before-ref 61ddff8` (the build the owner first tested). A preset missing in the old
  revision falls back to one of the same category and subcategory.
* **Genesis TL shown in dB truncated to "-22."** in the grid: reverted to raw steps.
* **Typing in the search field played notes in Renoise**: `EDITOR_WANTS_KEYBOARD_FOCUS`
  changes nothing for the Windows VST3; the field itself must report every key as
  handled (see `plugin/src/ui/KeyboardFocus.*` and `CommonStrip`).
* **pluginval.exe is a GUI-subsystem program**: `&` returns at once; the script uses
  `Start-Process -Wait` to get the exit code.
* **Git commit messages in PowerShell** break on quoting: use Git Bash and a heredoc.
* **CRLF rewrites** by editing tools inflate diffs: see the line-ending convention in
  `CLAUDE.md`.
* **Disk use**: each plugin build directory is about 1.3 GB; old agent build
  directories and regenerable reports were deleted on 2026-09-30 (project down from
  about 4.6 GB to about 2 GB).

## Open questions

* JUCE licence choice (see next steps).
* Were Renoise and the 360 Hz display validated by the owner? (see next steps)
* Genesis FM lead and organ presets live in `FM Brass` and `FM Pad` with tags; new
  `FM Lead` / `FM Organ` categories are an open product decision
  (`docs/HARDWARE_NOTES.md`).
* Items recorded as not verified in `README.md`, "Known limitations": the YM2612 LFO
  divider table could not be re-read from its only source; the MIDI Channel Mode scope
  follows general knowledge of MIDI 1.0; the pluginval VST3 validator step is skipped;
  several hardware details are not modelled (Genesis Model 2 filter, YM2612 timers and
  CSM, Famicom output stage, NES $4011 write collision...).

## Local files that are not in git

* `third_party/JUCE` (JUCE 9.0.3), `third_party/cc0` (CC0 source recordings, re-fetched
  by `python tools\gen_samples.py --fetch-cc0`), `third_party/refemu` (reference
  emulator binaries, URLs and hashes in `docs/research/reference-emulators.md`).
* `build/windows-x64-release`, `build/dsp-only-release`, `build-reports/` (listening
  set, screenshots, pluginval reports, refcheck results): all regenerable.
* The Claude memory of the previous account (user profile, decisions, toolchain notes):
  its durable content is folded into `CLAUDE.md` and this file.
