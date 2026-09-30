#include "chipdsp/util/BandLimitedStepSynth.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace chipdsp
{

namespace
{
    // Zeroth-order modified Bessel function of the first kind (series expansion).
    double besselI0(double x)
    {
        double sum = 1.0;
        double term = 1.0;
        const double halfX = x * 0.5;
        for (int k = 1; k < 64; ++k)
        {
            const double t = halfX / static_cast<double>(k);
            term *= t * t;
            sum += term;
            if (term < 1e-12 * sum)
                break;
        }
        return sum;
    }

    double kaiserWindow(double n, double halfLength, double beta)
    {
        const double r = n / halfLength;
        if (r <= -1.0 || r >= 1.0)
            return 0.0;
        return besselI0(beta * std::sqrt(1.0 - r * r)) / besselI0(beta);
    }

    double sinc(double x)
    {
        if (std::abs(x) < 1e-9)
            return 1.0;
        const double px = std::numbers::pi * x;
        return std::sin(px) / px;
    }
} // namespace

void BandLimitedStepSynth::prepare(double nativeRateHz, double hostRateHz, int maxBlock, Kernel kernelChoice)
{
    nativeRate = nativeRateHz;
    hostRate = hostRateHz;
    kernelType = kernelChoice;
    samplesPerClock = hostRateHz / nativeRateHz;
    bufferLen = maxBlock + kTaps + 2;
    buffer.assign(static_cast<size_t>(bufferLen), 0.0f);
    kernel.assign(static_cast<size_t>(kPhases * kTaps), 0.0f);
    buildKernel();
    reset();
}

void BandLimitedStepSynth::buildKernel()
{
    // Cutoff in cycles per host sample. When downsampling (native > host) the kernel is
    // the anti-aliasing filter: 0.45 keeps the transition band inside Nyquist for a
    // 32-tap Kaiser window. When upsampling (native < host) the cutoff sits at the native
    // Nyquist so that zero-order-hold images above it are removed, which reproduces what
    // the console's analog reconstruction filter does to the DAC's held output.
    const double ratio = nativeRate / hostRate;
    const double cutoff = ratio >= 1.0 ? 0.45 : 0.5 * ratio;
    constexpr double beta = 7.0; // ~ -70 dB side lobes

    auto impulse = [&](double x) {
        return 2.0 * cutoff * sinc(2.0 * cutoff * x) * kaiserWindow(x, static_cast<double>(kHalfTaps), beta);
    };

    if (kernelType == Kernel::IntegratedStep)
    {
        // Band-limited step S(x) = integral of the windowed sinc from -kHalfTaps to x, on a grid
        // of 1/kPhases host samples (Simpson's rule, 8 sub-intervals per cell), normalised so
        // that it rises from exactly 0 to exactly 1 across the window.
        constexpr int kGrid = kTaps * kPhases;
        constexpr int kSub = 8;
        std::vector<double> step(static_cast<size_t>(kGrid + 1), 0.0);
        const double cell = 1.0 / static_cast<double>(kPhases);
        const double h = cell / kSub;
        for (int g = 0; g < kGrid; ++g)
        {
            const double x0 = -static_cast<double>(kHalfTaps) + g * cell;
            double area = impulse(x0) + impulse(x0 + cell);
            for (int j = 1; j < kSub; ++j)
                area += (j & 1 ? 4.0 : 2.0) * impulse(x0 + j * h);
            step[static_cast<size_t>(g + 1)] = step[static_cast<size_t>(g)] + area * h / 3.0;
        }
        const double total = step[static_cast<size_t>(kGrid)];
        auto stepAt = [&](int g) { return g <= 0 ? 0.0 : step[static_cast<size_t>(std::min(g, kGrid))] / total; };

        for (int p = 0; p < kPhases; ++p)
        {
            // Event at fractional position frac = p / kPhases inside sample 0: output sample
            // m = k - kHalfTaps + 1 must read S(m - frac), so tap k is S(m - frac) - S(m - 1 - frac).
            // Grid index of x = m - frac is (m + kHalfTaps) * kPhases - p = (k + 1) * kPhases - p.
            double sum = 0.0;
            for (int k = 0; k < kTaps; ++k)
            {
                const double tap = stepAt((k + 1) * kPhases - p) - stepAt(k * kPhases - p);
                kernel[static_cast<size_t>(p * kTaps + k)] = static_cast<float>(tap);
                sum += tap;
            }
            // The part of the step beyond the last tap (phases p > 0) goes into the last tap, so
            // that a step of 'delta' integrates to exactly 'delta'.
            kernel[static_cast<size_t>(p * kTaps + kTaps - 1)] += static_cast<float>(1.0 - sum);
        }
        return;
    }

    // ImpulseSum (legacy, chiptool regs nes --kernel impulse only): sampled impulses, integrated by the running sum in endBlock().
    for (int p = 0; p < kPhases; ++p)
    {
        // Impulse located at fractional position frac = p / kPhases inside sample 0.
        // Output sample m (m = -kHalfTaps+1 .. kHalfTaps) sees h(m - frac).
        const double frac = static_cast<double>(p) / static_cast<double>(kPhases);
        double sum = 0.0;
        for (int k = 0; k < kTaps; ++k)
        {
            const int m = k - kHalfTaps + 1;
            const double x = static_cast<double>(m) - frac;
            const double h = impulse(x);
            kernel[static_cast<size_t>(p * kTaps + k)] = static_cast<float>(h);
            sum += h;
        }
        // Unit DC gain per phase, so that a step of 'delta' integrates to exactly 'delta'.
        const float norm = static_cast<float>(1.0 / sum);
        for (int k = 0; k < kTaps; ++k)
            kernel[static_cast<size_t>(p * kTaps + k)] *= norm;
    }
}

void BandLimitedStepSynth::reset() noexcept
{
    std::fill(buffer.begin(), buffer.end(), 0.0f);
    integrator = 0.0;
    rawLevel = 0.0f;
    rawLastTime = 0.0;
}

void BandLimitedStepSynth::addDelta(double tHostSamples, float delta) noexcept
{
    if (rawMode)
    {
        // Zero-order hold: every host sample instant in [rawLastTime, t) keeps the old level.
        const int from = static_cast<int>(std::ceil(rawLastTime));
        const int to = std::min(static_cast<int>(std::ceil(tHostSamples)), bufferLen);
        for (int i = std::max(from, 0); i < to; ++i)
            buffer[static_cast<size_t>(i)] = rawLevel;
        rawLastTime = std::max(rawLastTime, tHostSamples);
        rawLevel += delta;
        return;
    }

    if (tHostSamples < 0.0)
        tHostSamples = 0.0;

    const double whole = std::floor(tHostSamples);
    const int index = static_cast<int>(whole);
    int phase = static_cast<int>((tHostSamples - whole) * kPhases);
    phase = std::clamp(phase, 0, kPhases - 1);

    const float* k = &kernel[static_cast<size_t>(phase * kTaps)];
    const int start = index - kHalfTaps + 1;

    // The first sample of the kernel that falls before the current block start belongs to
    // the past and cannot be output any more; we fold it into the integrator so that the
    // final level stays exact.
    for (int tap = 0; tap < kTaps; ++tap)
    {
        const int i = start + tap;
        const float v = k[tap] * delta;
        if (i < 0)
            integrator += v;
        else if (i < bufferLen)
            buffer[static_cast<size_t>(i)] += v;
    }
}

void BandLimitedStepSynth::endBlock(float* out, int numSamples) noexcept
{
    if (rawMode)
    {
        const int from = std::max(static_cast<int>(std::ceil(rawLastTime)), 0);
        for (int i = from; i < numSamples; ++i)
            buffer[static_cast<size_t>(i)] = rawLevel;
        for (int i = 0; i < numSamples; ++i)
            out[i] += buffer[static_cast<size_t>(i)];
        rawLastTime = std::max(0.0, rawLastTime - static_cast<double>(numSamples));
        integrator = rawLevel;

        // Keep any samples already written past the block end (events scheduled in the future).
        const int remainingRaw = bufferLen - numSamples;
        std::copy(buffer.begin() + numSamples, buffer.end(), buffer.begin());
        std::fill(buffer.begin() + remainingRaw, buffer.end(), 0.0f);
        return;
    }

    // Integrate: the buffer holds band-limited impulses, the running sum is the level.
    // The integrator is exact (double, no leak); every engine follows this stage with a
    // DC-blocking high-pass modelling the console's output coupling, which also absorbs
    // any residual float rounding after very long sessions.
    for (int i = 0; i < numSamples; ++i)
    {
        integrator += static_cast<double>(buffer[static_cast<size_t>(i)]);
        out[i] += static_cast<float>(integrator);
    }

    // Shift the tail (future contributions) to the front.
    const int remaining = bufferLen - numSamples;
    std::copy(buffer.begin() + numSamples, buffer.end(), buffer.begin());
    std::fill(buffer.begin() + remaining, buffer.end(), 0.0f);
}

void BandLimitedStepSynth::endBlockReplace(float* out, int numSamples) noexcept
{
    std::fill(out, out + numSamples, 0.0f);
    endBlock(out, numSamples);
}

} // namespace chipdsp
