#pragma once

#include "chipdsp/ChipTypes.h"

#include <span>

namespace chipdsp
{

// Common contract implemented by Nes2A03Engine, SnesDspEngine and GenesisEngine.
//
// Threading model
//   prepare()/loadSample()/parameterDescriptors(): message thread, may allocate.
//   Everything else: audio thread. No allocation, no locks, no I/O, no exceptions.
//
// Timing model
//   Every engine runs its chip at the native clock and resamples to hostSampleRate
//   internally (see docs/ARCHITECTURE.md). renderBlock() is called with sub-blocks
//   split at MIDI event boundaries by the plugin layer, so noteOn/noteOff/
//   setChannelPitch/setParameter take effect at the start of the next renderBlock().
//
// Channels
//   'channel' always indexes the chip's hardware channels (see ChipTypes.h).
//   Pitch is a MIDI note number (float, 69 = A4 = 440 Hz); each engine quantises
//   it to its own register resolution (11-bit period, 14-bit pitch, block/fnum...).
//
// Output
//   mainL/mainR receive the chip's real summed output (non-linear mixer, echo,
//   DAC quirks included). channelOutsL/R, when non-null, receive one stereo pair
//   per hardware channel rendered as if the other channels were silent. Because
//   the NES mixer is non-linear these per-channel signals do not sum to the main
//   output; they are monitoring/multi-out feeds.
class IChipEngine
{
public:
    virtual ~IChipEngine() = default;

    virtual ChipId chipId() const noexcept = 0;
    virtual int numChannels() const noexcept = 0;
    virtual ChannelInfo channelInfo(int channel) const noexcept = 0;

    // Native chip output rate for the current clock standard (Hz), e.g. 32000 for the S-DSP.
    virtual double nativeSampleRate() const noexcept = 0;

    // ----- lifecycle (message thread) -------------------------------------------------------
    virtual void prepare(double hostSampleRate, int maxBlockSize) = 0;
    virtual std::span<const ParamDesc> parameterDescriptors() const noexcept = 0;

    // Replace the sample stored in 'slot' with mono PCM (float, -1..1). The engine encodes to
    // its native format (DMC 1-bit delta, BRR ADPCM, 8-bit DAC PCM). Returns false if the slot
    // is invalid or the sample does not fit the hardware memory budget.
    // Must not be called concurrently with itself; safe against a running renderBlock().
    virtual bool loadSample(int slot, const float* mono, int numFrames, double sourceSampleRate) = 0;
    virtual int numSampleSlots() const noexcept = 0;

    // Optional playback metadata for a slot filled by loadSample() (message thread).
    // rootNote: MIDI note that plays the sample at its source pitch. loopStartFrame: first
    // frame of the loop in the PCM passed to loadSample() (the loop runs to the end), or -1
    // for a one-shot. sourceSampleRate: the rate given to loadSample(). Engines whose
    // hardware has no such notion (DMC, YM2612 DAC: pitch comes from the rate parameter)
    // keep the default and return false.
    virtual bool setSampleInfo(int slot, float rootNote, int loopStartFrame, double sourceSampleRate)
    {
        (void) slot; (void) rootNote; (void) loopStartFrame; (void) sourceSampleRate;
        return false;
    }

    // ----- audio thread ----------------------------------------------------------------------
    virtual void reset() noexcept = 0;   // silence everything, clear delay lines, keep parameters

    virtual void setParameter(int id, float value) noexcept = 0;
    virtual float getParameter(int id) const noexcept = 0;

    virtual void noteOn(int channel, float midiNote, float velocity) noexcept = 0; // velocity 0..1
    virtual void noteOff(int channel) noexcept = 0;
    virtual void setChannelPitch(int channel, float midiNote) noexcept = 0;        // glide / bend
    virtual bool isChannelActive(int channel) const noexcept = 0;                  // for voice stealing

    virtual void setClockStandard(ClockStandard standard) noexcept = 0;            // S-DSP ignores it
    virtual void setRawOutput(bool raw) noexcept = 0;                              // bypass band-limiting

    // channelOutsL/R: arrays of numChannels() pointers, or nullptr to skip per-channel rendering.
    virtual void renderBlock(float* mainL, float* mainR,
                             float* const* channelOutsL, float* const* channelOutsR,
                             int numSamples) noexcept = 0;
};

} // namespace chipdsp
