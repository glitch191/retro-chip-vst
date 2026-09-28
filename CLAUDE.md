# retro-chip-vst

Hardware-faithful NES (2A03), SNES (S-DSP) and Sega Genesis (YM2612 + SN76489) synthesis
VST3 for Windows x64. C++20, CMake, JUCE 9 (plugin layer only), Catch2 v3 (DSP tests),
Python (generators).

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
* `dsp/` (library `chipdsp`) must never include JUCE. It builds and tests alone.
* Audio thread (`renderBlock`, `noteOn`, `noteOff`, `setParameter`, `reset`...): no
  allocation, no locks, no I/O, no exceptions. Tables are precomputed in `prepare()` or
  as `constexpr`/static data.
* Keep the code simple and readable. No premature abstraction.
* Do not run `git` commands from subagents; the orchestrator commits.

## Layout

```
dsp/include/chipdsp/   public headers (IChipEngine.h, ChipTypes.h, util/, nes/, snes/, genesis/, perf/)
dsp/src/<area>/        implementation, one static library per area (see dsp/CMakeLists.txt)
dsp/tests/             Catch2 tests: test_<area>_*.cpp -> chipdsp_tests_<area>
plugin/src/            JUCE layer
tools/                 Python generators, dev-env.ps1
assets/                generated presets and samples
docs/                  ARCHITECTURE.md, HARDWARE_NOTES.md, SOURCES.md, PRESET_QA.md, research/
```

## Build and test (Windows PowerShell)

```powershell
. .\tools\dev-env.ps1                      # MSVC x64 env + bundled cmake/ninja/git
cmake --preset dsp-only-release            # chipdsp + tests, no JUCE
cmake --build --preset dsp-only-release --target chipdsp_tests_nes
ctest --preset dsp-only-release -R nes     # or run build\dsp-only-release\dsp\tests\chipdsp_tests_nes.exe
.\build.ps1                                # full build including the VST3
```

Subagents working in parallel must use a private build directory instead of the preset:

```powershell
. .\tools\dev-env.ps1
cmake -S . -B build\agent-<name> -G Ninja -DCMAKE_BUILD_TYPE=Release -DRCV_BUILD_PLUGIN=OFF -DRCV_BUILD_BENCH=OFF
cmake --build build\agent-<name> --target chipdsp_tests_<area>
build\agent-<name>\dsp\tests\chipdsp_tests_<area>.exe
```

Only build the targets of your own area; other areas may be mid-edit by another agent.
