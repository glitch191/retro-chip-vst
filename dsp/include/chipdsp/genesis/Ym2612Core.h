#pragma once

// Register-level model of the Yamaha YM2612 (OPN2) as used in the Sega Genesis.
// No notion of MIDI: everything is driven by register writes and clockSample(), which
// advances exactly one FM output sample (144 YM clocks). Section names in comments refer
// to docs/research/genesis.md.
//
// Operator indices are Yamaha slot numbers S1..S4 (0..3), not register order.

#include "chipdsp/genesis/GenesisTables.h"

#include <cstdint>

namespace chipdsp::genesis
{

class Ym2612Core
{
public:
    enum class EgPhase : uint8_t { Attack, Decay, Sustain, Release };

    Ym2612Core() noexcept { reset(); }

    // Power-on state: all registers 0 except L/R output enables (set, research "DAC registers"),
    // every operator released at maximum attenuation.
    void reset() noexcept;

    // Port interface as seen by the 68000/Z80: port 0 = bank 0 address, 1 = bank 0 data,
    // 2 = bank 1 address, 3 = bank 1 data. A data write goes to the bank last addressed.
    void writePort(int port, uint8_t value) noexcept;
    // Address + data in one call (bank 0 or 1).
    void write(int bank, uint8_t reg, uint8_t value) noexcept;

    // Advance one FM sample: LFO, SSG-EG logic, envelope (every 3rd sample), operators.
    void clockSample() noexcept;

    // Discrete YM2612 ladder effect (chip_revision 0) or linear YM3438 / ASIC output (1).
    void setLadderEffect(bool on) noexcept { ladder = on; }
    bool ladderEffect() const noexcept { return ladder; }

    // Latest 9-bit channel sample (-256..255), with the DAC substituted on channel 6.
    int channelOutput(int ch) const noexcept { return channels[ch].out9; }
    bool channelLeft(int ch) const noexcept { return (channels[ch].panAmsFms & 0x80) != 0; }
    bool channelRight(int ch) const noexcept { return (channels[ch].panAmsFms & 0x40) != 0; }
    // Output-stage contribution of one channel to one side (research "Ladder effect").
    int channelOutputLeft(int ch) const noexcept { return ladderOutput(channels[ch].out9, channelLeft(ch), ladder); }
    int channelOutputRight(int ch) const noexcept { return ladderOutput(channels[ch].out9, channelRight(ch), ladder); }
    // Sum of the six channel contributions, 9-bit steps.
    int outputLeft() const noexcept;
    int outputRight() const noexcept;

    // True when no operator of the channel is keyed and every carrier is silent.
    bool channelIdle(int ch) const noexcept;

    // ----- inspection (tests, visualisation) -------------------------------------------------
    int operatorAttenuation(int ch, int op) const noexcept { return channels[ch].op[op].att; }
    EgPhase operatorPhase(int ch, int op) const noexcept { return channels[ch].op[op].phase; }
    int operatorEgOutput(int ch, int op) const noexcept { return egOutput(channels[ch], channels[ch].op[op]); }
    int operatorLastOutput(int ch, int op) const noexcept { return channels[ch].op[op].out; }
    uint32_t operatorPhaseIncrement(int ch, int op) const noexcept;
    uint32_t operatorPhaseCounter(int ch, int op) const noexcept { return channels[ch].op[op].phase20; }
    int channelKeyCode(int ch) const noexcept { return keyCode(channels[ch].block, channels[ch].fnum); }
    int channelFnum(int ch) const noexcept { return channels[ch].fnum; }
    int channelBlock(int ch) const noexcept { return channels[ch].block; }
    int lfoCounter() const noexcept { return lfo; }
    int egCounter() const noexcept { return egCount; }
    bool dacEnabled() const noexcept { return dacOn; }

private:
    struct Operator
    {
        // registers
        uint8_t dt = 0, mul = 0, tl = 0, rs = 0, ar = 0, amOn = 0, dr = 0, sr = 0, sl = 0, rr = 0, ssg = 0;
        // state
        EgPhase phase = EgPhase::Release;
        bool keyed = false;
        bool ssgInvert = false;
        uint16_t att = kEgMaxAttenuation;   // 10-bit attenuation
        uint32_t phase20 = 0;               // 20-bit phase counter
        int out = 0;                        // last 14-bit output
        int prevOut = 0;                    // output of the previous sample (pipeline)
    };

    struct Channel
    {
        Operator op[4];
        uint16_t fnum = 0;
        uint8_t block = 0;
        uint8_t fnumHighLatch = 0;          // $A4 value waiting for the $A0 write
        uint8_t algorithm = 0;
        uint8_t feedback = 0;
        uint8_t panAmsFms = 0xC0;           // $B4 layout: L R AMS1 AMS0 0 FMS2 FMS1 FMS0
        int fb1 = 0, fb2 = 0;               // S1 outputs n-1 and n-2 (feedback)
        int out9 = 0;
    };

    void writeRegister(int bank, uint8_t reg, uint8_t value) noexcept;
    void writeKeyOnOff(uint8_t value) noexcept;
    void keyOn(Channel& ch, Operator& op) noexcept;
    void keyOff(Operator& op) noexcept;

    int attackRate(const Channel& ch, const Operator& op) const noexcept;
    int currentRate(const Channel& ch, const Operator& op) const noexcept;
    int egOutput(const Channel& ch, const Operator& op) const noexcept;
    void ssgUpdate(Channel& ch, Operator& op) noexcept;
    void egUpdate(Channel& ch, Operator& op) noexcept;
    int computeChannel(Channel& ch) noexcept;
    uint32_t phaseIncrement(const Channel& ch, const Operator& op) const noexcept;

    Channel channels[6];
    uint8_t address = 0;       // single address latch (research "Registers": one data port)
    int addressBank = 0;

    // global registers
    bool lfoOn = false;
    uint8_t lfoFreq = 0;
    uint8_t ch3Mode = 0;       // $27 bits 7-6, stored only (special mode / CSM not modelled)
    bool dacOn = false;
    uint8_t dacData = 0x80;
    bool ladder = true;

    // counters
    int lfo = 0;               // 7-bit LFO counter
    int lfoDivider = 0;
    int egSub = 0;             // FM samples within the 3-sample EG cycle
    int egCount = 0;           // 12-bit global EG counter (skips 0)
};

} // namespace chipdsp::genesis
