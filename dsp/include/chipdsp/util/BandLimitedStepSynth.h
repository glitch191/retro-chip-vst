#pragma once

#include <cstdint>
#include <vector>

namespace chipdsp
{

// Band-limited step synthesis (native clock -> host sample rate).
//
// Every chip in this project produces a piecewise-constant signal at its native
// clock (1.79 MHz for the 2A03, 32 kHz for the S-DSP, 53 kHz for the YM2612,
// 224 kHz for the SN76489). The engine reports each level change with its exact
// fractional position in host samples; this class adds a windowed-sinc impulse
// scaled by the change into an accumulation buffer, and endBlock() integrates the
// buffer into the band-limited step signal. See docs/ARCHITECTURE.md ("Resampling").
//
// In raw mode the native signal is sampled with a zero-order hold at every host
// sample instant instead (nearest previous level), which preserves the aliasing a
// real console output would show through a wide analog path.
//
// Kernel choice (docs/research/refcheck-report.md, finding F2):
//  * IntegratedStep: each kernel tap is the first difference of the integrated windowed sinc,
//    so the running sum in endBlock() gives exact samples of the band-limited step. Flat pass
//    band. The default, used by every engine (the NES switched to it on 2026-09-30,
//    product-owner decision).
//  * ImpulseSum: taps are samples of the windowed sinc itself; the discrete running sum then
//    boosts the top octave by (w/2) / sin(w/2), w = 2 pi f / host rate (+0.94 dB at 11.2 kHz
//    at 44.1 kHz). Legacy kernel of the NES engine before 2026-09-30, kept only for
//    `chiptool regs nes --kernel impulse` (the refcheck F2 measurement) and its regression test.
//
// Real-time safety: prepare() allocates; everything else is allocation-free.
class BandLimitedStepSynth
{
public:
    enum class Kernel
    {
        ImpulseSum,
        IntegratedStep,
    };

    static constexpr int kPhases = 64;     // fractional-time resolution (1/64 host sample)
    static constexpr int kTaps = 32;       // kernel length in host samples
    static constexpr int kHalfTaps = kTaps / 2;

    BandLimitedStepSynth() = default;

    // nativeRateHz: chip clock at which level changes can occur.
    // hostRateHz  : output sample rate.
    // maxBlock    : largest numSamples passed to endBlock().
    // kernel      : see the class comment.
    void prepare(double nativeRateHz, double hostRateHz, int maxBlock, Kernel kernel = Kernel::IntegratedStep);

    void reset() noexcept;
    void setRaw(bool raw) noexcept { rawMode = raw; }
    bool isRaw() const noexcept { return rawMode; }

    // Host samples elapsed per native clock (host / native).
    double hostSamplesPerClock() const noexcept { return samplesPerClock; }

    // Register a level change of 'delta' at host time 'tHostSamples' (block-relative,
    // may be >= numSamples of the current block by up to kTaps; must be non-decreasing
    // within a block). Audio thread.
    void addDelta(double tHostSamples, float delta) noexcept;

    // Produce numSamples output samples (added to 'out', not replaced) and shift the
    // pending tail so the next block starts at time 0. Audio thread.
    void endBlock(float* out, int numSamples) noexcept;

    // Same as endBlock but overwrites 'out'.
    void endBlockReplace(float* out, int numSamples) noexcept;

    // Current integrated level (useful for tests / DC inspection).
    float currentLevel() const noexcept { return static_cast<float>(integrator); }

private:
    void buildKernel();

    double samplesPerClock = 1.0;
    double nativeRate = 1.0;
    double hostRate = 1.0;
    Kernel kernelType = Kernel::IntegratedStep;
    bool rawMode = false;

    std::vector<float> kernel;      // kPhases * kTaps, each phase sums to exactly 1
    std::vector<float> buffer;      // maxBlock + kTaps + 1 accumulation samples
    int bufferLen = 0;
    double integrator = 0.0;        // running sum = band-limited level

    // Raw mode state
    float rawLevel = 0.0f;          // current native level
    double rawLastTime = 0.0;       // last event time within the block
};

} // namespace chipdsp
