#pragma once

#include <cstddef>
#include <cstdint>

namespace chipdsp
{

enum class ChipId : int
{
    Nes = 0,
    Snes = 1,
    Genesis = 2
};

enum class ClockStandard : int
{
    Ntsc = 0,
    Pal = 1
};

// Largest hardware channel count across the three chips (Genesis: 6 FM + 4 PSG).
constexpr int kMaxHardwareChannels = 10;

// Hardware channel counts (polyphony ceilings enforced by the arpeggiator and voice allocator).
constexpr int kNesChannels = 5;      // pulse1, pulse2, triangle, noise, dmc
constexpr int kSnesChannels = 8;     // voice 0..7
constexpr int kGenesisFmChannels = 6;
constexpr int kGenesisPsgChannels = 4; // tone1..3, noise
constexpr int kGenesisChannels = kGenesisFmChannels + kGenesisPsgChannels;

struct ChannelInfo
{
    const char* name;       // "Pulse 1", "Voice 3", "FM 6", "PSG Noise"
    const char* shortName;  // "P1", "V3", "FM6", "NZ" (used in dense UI / bus names)
    bool isPitched;         // false for NES noise/DMC and PSG noise when keyed by period
};

// Description of one engine parameter, in native hardware units.
// The plugin layer builds host parameters from these and the randomizer never
// leaves [minValue, maxValue]. Integer parameters carry whole numbers in a float.
struct ParamDesc
{
    int id;                 // engine-specific enum value
    const char* key;        // stable identifier, snake_case ASCII ("pulse1_duty")
    const char* name;       // display name ("Pulse 1 Duty")
    const char* group;      // UI grouping ("Pulse 1", "Envelope", "Operator 2", "Echo")
    float minValue;
    float maxValue;
    float defaultValue;
    bool isInteger;         // true: discrete register value; false: continuous (ms, Hz, ...)
    const char* unit;       // "" or "ms", "Hz", "dB", "%" ...
    const char* const* choiceLabels; // nullptr, or (maxValue-minValue+1) labels for enumerated ints
};

// Host transport snapshot handed to tempo-synced modules. Plain data, no framework types.
struct TransportInfo
{
    double bpm = 120.0;
    double ppqPosition = 0.0;   // quarter notes since start
    bool isPlaying = false;
};

} // namespace chipdsp
