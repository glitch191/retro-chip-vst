#pragma once

#include <cmath>
#include <numbers>

namespace chipdsp
{

// First-order high-pass (RC coupling capacitor model). Used for DC blocking and for
// the documented console output stages (NES 90 Hz / 440 Hz).
class OnePoleHighPass
{
public:
    void prepare(double cutoffHz, double sampleRate) noexcept
    {
        const double rc = 1.0 / (2.0 * std::numbers::pi * cutoffHz);
        const double dt = 1.0 / sampleRate;
        alpha = static_cast<float>(rc / (rc + dt));
        reset();
    }
    void reset() noexcept { prevIn = 0.0f; prevOut = 0.0f; }
    float process(float in) noexcept
    {
        const float out = alpha * (prevOut + in - prevIn);
        prevIn = in;
        prevOut = out;
        return out;
    }
private:
    float alpha = 1.0f;
    float prevIn = 0.0f;
    float prevOut = 0.0f;
};

// First-order low-pass (RC), used for documented console output stages (NES 14 kHz).
class OnePoleLowPass
{
public:
    void prepare(double cutoffHz, double sampleRate) noexcept
    {
        const double rc = 1.0 / (2.0 * std::numbers::pi * cutoffHz);
        const double dt = 1.0 / sampleRate;
        alpha = static_cast<float>(dt / (rc + dt));
        reset();
    }
    void reset() noexcept { state = 0.0f; }
    float process(float in) noexcept
    {
        state += alpha * (in - state);
        return state;
    }
private:
    float alpha = 1.0f;
    float state = 0.0f;
};

} // namespace chipdsp
