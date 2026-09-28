#include "EngineHost.h"

#include "MidiLearn.h"

#include "chipdsp/EngineFactory.h"

#include <algorithm>
#include <cmath>

namespace rcv
{

namespace
{
    constexpr float kPi = 3.14159265358979323846f;

    int chipIndex (chipdsp::ChipId chip) noexcept { return static_cast<int> (chip); }

    bool sameArpParams (const chipdsp::Arpeggiator::Params& a, const chipdsp::Arpeggiator::Params& b) noexcept
    {
        return a.enabled == b.enabled && a.pattern == b.pattern && a.octaves == b.octaves
            && a.rateMode == b.rateMode && a.syncDivision == b.syncDivision
            && a.freeRateHz == b.freeRateHz && a.gatePercent == b.gatePercent && a.hold == b.hold;
    }

    bool sameGlideParams (const chipdsp::Glide::Params& a, const chipdsp::Glide::Params& b) noexcept
    {
        return a.timeMs == b.timeMs && a.mode == b.mode;
    }
} // namespace

EngineHost::EngineHost()
{
    for (int c = 0; c < kNumChips; ++c)
        engines[static_cast<size_t> (c)] = chipdsp::createEngine (static_cast<chipdsp::ChipId> (c));

    sustain.fill (false);
    sustained.fill (false);
    pitchValid.fill (false);
    lastPitch.fill (0.0f);
    bend.fill (0.0f);
    scopes.setActiveChannelCount (activeEngine().numChannels());
}

EngineHost::~EngineHost() = default;

// ----- message thread ------------------------------------------------------------------------

void EngineHost::attachParameters (const ParamRegistry& registry, juce::AudioProcessorValueTreeState& apvts)
{
    for (int c = 0; c < kNumChips; ++c)
    {
        auto& list = links[static_cast<size_t> (c)];
        list.clear();
        for (const auto* info : registry.engineParams (static_cast<chipdsp::ChipId> (c)))
        {
            ParamLink link;
            link.raw = apvts.getRawParameterValue (info->id);
            link.engineId = info->engineParamId;
            link.offset = info->isChoice() ? info->desc.minValue : 0.0f;
            if (link.raw != nullptr)
                list.push_back (link);
        }
    }

    chipParam = apvts.getRawParameterValue (ParamIds::chip);
    rawOutputParam = apvts.getRawParameterValue (ParamIds::rawOutput);
    voiceModeParam = apvts.getRawParameterValue (ParamIds::voiceMode);
    polyChannelsParam = apvts.getRawParameterValue (ParamIds::polyChannels);
    masterGainParam = apvts.getRawParameterValue (ParamIds::masterGain);
    arpEnabledParam = apvts.getRawParameterValue (ParamIds::arpEnabled);
    arpPatternParam = apvts.getRawParameterValue (ParamIds::arpPattern);
    arpOctavesParam = apvts.getRawParameterValue (ParamIds::arpOctaves);
    arpRateModeParam = apvts.getRawParameterValue (ParamIds::arpRateMode);
    arpSyncDivisionParam = apvts.getRawParameterValue (ParamIds::arpSyncDivision);
    arpFreeRateParam = apvts.getRawParameterValue (ParamIds::arpFreeRate);
    arpGateParam = apvts.getRawParameterValue (ParamIds::arpGate);
    arpHoldParam = apvts.getRawParameterValue (ParamIds::arpHold);
    glideTimeParam = apvts.getRawParameterValue (ParamIds::glideTime);
    glideModeParam = apvts.getRawParameterValue (ParamIds::glideMode);
    jassert (chipParam != nullptr && masterGainParam != nullptr);

    const int idx = juce::jlimit (0, kNumChips - 1, static_cast<int> (readRaw (chipParam, 0.0f)));
    current = static_cast<chipdsp::ChipId> (idx);
    activeChipIndex.store (idx, std::memory_order_relaxed);
    scopes.setActiveChannelCount (activeEngine().numChannels());
}

void EngineHost::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    maxBlock = std::max (1, maxBlockSize);

    for (auto& e : engines)
        e->prepare (sampleRate, maxBlock);

    fadeLength = std::max (1, static_cast<int> (std::lround (kCrossfadeSeconds * sampleRate)));
    fadeIn.assign (static_cast<size_t> (fadeLength), 0.0f);
    fadeOut.assign (static_cast<size_t> (fadeLength), 0.0f);
    for (int i = 0; i < fadeLength; ++i)
    {
        const float t = (static_cast<float> (i) + 1.0f) / static_cast<float> (fadeLength);
        fadeIn[static_cast<size_t> (i)] = std::sin (t * kPi * 0.5f);
        fadeOut[static_cast<size_t> (i)] = std::cos (t * kPi * 0.5f);
    }

    spareR.assign (static_cast<size_t> (maxBlock), 0.0f);
    fadeMainL.assign (static_cast<size_t> (maxBlock), 0.0f);
    fadeMainR.assign (static_cast<size_t> (maxBlock), 0.0f);
    for (int c = 0; c < kMaxChannels; ++c)
    {
        const auto i = static_cast<size_t> (c);
        chanBlockL[i].assign (static_cast<size_t> (maxBlock), 0.0f);
        chanBlockR[i].assign (static_cast<size_t> (maxBlock), 0.0f);
        fadeChanL[i].assign (static_cast<size_t> (maxBlock), 0.0f);
        fadeChanR[i].assign (static_cast<size_t> (maxBlock), 0.0f);
    }

    for (auto& list : links)
        for (auto& link : list)
            link.pushed = false;
    arpParamsValid = false;
    glideParamsValid = false;
    rawOutputValid = false;

    prepared = true;
    reset();
}

bool EngineHost::loadUserSample (chipdsp::ChipId chip, int slot, const juce::AudioBuffer<float>& audio, double sourceSampleRate)
{
    const int numFrames = audio.getNumSamples();
    const int numChannels = audio.getNumChannels();
    if (numFrames <= 0 || numChannels <= 0 || sourceSampleRate <= 0.0)
        return false;

    std::vector<float> mono (static_cast<size_t> (numFrames), 0.0f);
    const float scale = 1.0f / static_cast<float> (numChannels);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* src = audio.getReadPointer (ch);
        for (int i = 0; i < numFrames; ++i)
            mono[static_cast<size_t> (i)] += src[i] * scale;
    }
    return engine (chip).loadSample (slot, mono.data(), numFrames, sourceSampleRate);
}

// ----- audio thread --------------------------------------------------------------------------

float EngineHost::readRaw (const std::atomic<float>* p, float fallback) const noexcept
{
    return p != nullptr ? p->load (std::memory_order_relaxed) : fallback;
}

void EngineHost::reset() noexcept
{
    for (auto& e : engines)
        e->reset();
    arp.reset();
    glider.reset();
    fading = false;
    crossfading.store (false, std::memory_order_relaxed);
    fadePos = 0;
    bend.fill (0.0f);
    sustain.fill (false);
    sustained.fill (false);
    pitchValid.fill (false);
    noteIn.clear();
    noteOut.clear();
    numControls = 0;
    gain = targetGain;
    configureAllocator (true);
}

void EngineHost::process (juce::AudioBuffer<float>& mainBus, juce::MidiBuffer& midi,
                          const chipdsp::TransportInfo& transport, const BusMap& buses) noexcept
{
    const int numSamples = mainBus.getNumSamples();
    if (! prepared || numSamples <= 0 || mainBus.getNumChannels() < 1)
    {
        mainBus.clear();
        return;
    }

    float* outL = mainBus.getWritePointer (0);
    float* outR = mainBus.getNumChannels() > 1 ? mainBus.getWritePointer (1) : nullptr;

    // Hosts normally respect the prepared block size; longer blocks are cut into slices.
    int start = 0;
    while (start < numSamples)
    {
        const int len = std::min (maxBlock, numSamples - start);

        chipdsp::TransportInfo sliceTransport = transport;
        if (start > 0 && transport.isPlaying)
            sliceTransport.ppqPosition += static_cast<double> (start) / sampleRate * transport.bpm / 60.0;

        BusMap sliceBuses;
        for (int c = 0; c < kMaxChannels; ++c)
        {
            const auto i = static_cast<size_t> (c);
            sliceBuses.left[i] = buses.left[i] != nullptr ? buses.left[i] + start : nullptr;
            sliceBuses.right[i] = buses.right[i] != nullptr ? buses.right[i] + start : nullptr;
        }

        processSlice (outL + start, outR != nullptr ? outR + start : spareR.data(), len, midi, start, sliceTransport, sliceBuses);
        start += len;
    }
}

void EngineHost::processSlice (float* outL, float* outR, int numSamples, const juce::MidiBuffer& midi, int sliceStart,
                               const chipdsp::TransportInfo& transport, const BusMap& buses) noexcept
{
    updateChipSelection();
    pushParameters();
    collectMidi (midi, sliceStart, numSamples);

    noteOut.clear();
    arp.process (transport, sampleRate, numSamples, noteIn.view(), noteOut);

    const bool wantChannels = buses.anyEnabled() || scopes.isEnabled();
    const int inCount = activeEngine().numChannels();
    const int outCount = fading ? engine (outgoing).numChannels() : 0;
    channelsThisBlock = std::max (inCount, outCount);
    if (wantChannels)
        for (int c = inCount; c < channelsThisBlock; ++c)
        {
            std::fill_n (chanBlockL[static_cast<size_t> (c)].data(), numSamples, 0.0f);
            std::fill_n (chanBlockR[static_cast<size_t> (c)].data(), numSamples, 0.0f);
        }

    int cursor = 0;
    int ctrlIdx = 0;
    int noteIdx = 0;
    const int numNotes = noteOut.size();

    while (cursor < numSamples)
    {
        while (ctrlIdx < numControls && controls[static_cast<size_t> (ctrlIdx)].offset <= cursor)
            applyControl (controls[static_cast<size_t> (ctrlIdx++)]);
        while (noteIdx < numNotes && noteOut[noteIdx].sampleOffset <= cursor)
            applyNote (noteOut[noteIdx++]);

        int next = numSamples;
        if (ctrlIdx < numControls)
            next = std::min (next, controls[static_cast<size_t> (ctrlIdx)].offset);
        if (noteIdx < numNotes)
            next = std::min (next, noteOut[noteIdx].sampleOffset);

        int len = std::max (1, next - cursor);
        if (fading || anyGlideActive())
            len = std::min (len, kControlIntervalSamples);
        len = std::min (len, numSamples - cursor);

        updatePitches();
        renderSubBlock (cursor, len, outL, outR, wantChannels);
        glider.advance (len, sampleRate);
        cursor += len;
    }

    // Events placed at or past the end of the slice take effect now (next block start).
    while (ctrlIdx < numControls)
        applyControl (controls[static_cast<size_t> (ctrlIdx++)]);
    while (noteIdx < numNotes)
        applyNote (noteOut[noteIdx++]);

    applyMasterGain (outL, outR, numSamples, wantChannels);
    deliverBuses (buses, numSamples, wantChannels);
    feedVisualizer (outL, outR, numSamples, wantChannels);
}

void EngineHost::updateChipSelection() noexcept
{
    const int idx = juce::jlimit (0, kNumChips - 1, static_cast<int> (readRaw (chipParam, static_cast<float> (chipIndex (current)))));
    const auto requested = static_cast<chipdsp::ChipId> (idx);
    if (requested != current)
        beginSwitch (requested);
}

void EngineHost::beginSwitch (chipdsp::ChipId target) noexcept
{
    if (fading)
    {
        // Finish the running fade at once: the previous outgoing engine falls silent.
        engine (outgoing).reset();
        fading = false;
    }

    outgoing = current;
    current = target;

    auto& out = engine (outgoing);
    for (int c = 0; c < out.numChannels(); ++c)
        out.noteOff (c);

    activeEngine().reset();   // the incoming engine starts silent
    fading = true;
    fadePos = 0;
    crossfading.store (true, std::memory_order_relaxed);

    glider.reset();
    sustained.fill (false);
    pitchValid.fill (false);
    configureAllocator (true);

    activeChipIndex.store (chipIndex (current), std::memory_order_relaxed);
    scopes.setActiveChannelCount (activeEngine().numChannels());
}

void EngineHost::pushParameters() noexcept
{
    for (int c = 0; c < kNumChips; ++c)
    {
        auto& e = *engines[static_cast<size_t> (c)];
        for (auto& link : links[static_cast<size_t> (c)])
        {
            const float v = link.raw->load (std::memory_order_relaxed);
            if (! link.pushed || v != link.last)
            {
                e.setParameter (link.engineId, v + link.offset);
                link.last = v;
                link.pushed = true;
            }
        }
    }

    const bool raw = readRaw (rawOutputParam, 0.0f) > 0.5f;
    if (! rawOutputValid || raw != lastRawOutput)
    {
        for (auto& e : engines)
            e->setRawOutput (raw);
        lastRawOutput = raw;
        rawOutputValid = true;
    }

    chipdsp::Arpeggiator::Params ap;
    ap.enabled = readRaw (arpEnabledParam, 0.0f) > 0.5f;
    ap.pattern = static_cast<chipdsp::Arpeggiator::Pattern> (juce::jlimit (0, 4, static_cast<int> (readRaw (arpPatternParam, 0.0f))));
    ap.octaves = juce::jlimit (1, chipdsp::Arpeggiator::kMaxOctaves, static_cast<int> (readRaw (arpOctavesParam, 1.0f)));
    ap.rateMode = static_cast<chipdsp::Arpeggiator::RateMode> (juce::jlimit (0, 1, static_cast<int> (readRaw (arpRateModeParam, 0.0f))));
    ap.syncDivision = static_cast<chipdsp::Arpeggiator::SyncDivision> (juce::jlimit (0, 5, static_cast<int> (readRaw (arpSyncDivisionParam, 3.0f))));
    ap.freeRateHz = readRaw (arpFreeRateParam, 8.0f);
    ap.gatePercent = readRaw (arpGateParam, 50.0f);
    ap.hold = readRaw (arpHoldParam, 0.0f) > 0.5f;
    if (! arpParamsValid || ! sameArpParams (ap, lastArpParams))
    {
        arp.setParams (ap);
        lastArpParams = ap;
        arpParamsValid = true;
    }

    chipdsp::Glide::Params gp;
    gp.timeMs = readRaw (glideTimeParam, 0.0f);
    gp.mode = static_cast<chipdsp::Glide::Mode> (juce::jlimit (0, 1, static_cast<int> (readRaw (glideModeParam, 0.0f))));
    if (! glideParamsValid || ! sameGlideParams (gp, lastGlideParams))
    {
        glider.setParams (gp);
        lastGlideParams = gp;
        glideParamsValid = true;
    }

    configureAllocator (false);

    targetGain = juce::Decibels::decibelsToGain (readRaw (masterGainParam, 0.0f), -60.0f);
}

void EngineHost::configureAllocator (bool force) noexcept
{
    const bool midiMode = static_cast<int> (readRaw (voiceModeParam, static_cast<float> (VoiceMode::Poly))) == static_cast<int> (VoiceMode::MidiChannel);
    const int count = activeEngine().numChannels();
    const uint32_t chipMask = ParamRegistry::chipChannelMask (current);
    uint32_t mask = chipMask;
    if (! midiMode)
    {
        const float rawMask = readRaw (polyChannelsParam, static_cast<float> (chipMask));
        mask = static_cast<uint32_t> (juce::jlimit (0, 1023, static_cast<int> (rawMask))) & chipMask;
        if (mask == 0)
            mask = ParamRegistry::defaultPolyMask (current) & chipMask;
    }

    if (force)
    {
        allocator.configure (count, mask);
        allocator.setMidiChannelMode (midiMode);
        lastMask = mask;
        lastMidiMode = midiMode;
        return;
    }

    if (midiMode != lastMidiMode)
    {
        // Notes are keyed differently in each mode: release everything, then reconfigure.
        silenceAll();
        allocator.configure (count, mask);
        allocator.setMidiChannelMode (midiMode);
        lastMask = mask;
        lastMidiMode = midiMode;
        return;
    }

    if (mask != lastMask)
    {
        allocator.setEnabledMask (mask);
        auto& e = activeEngine();
        for (int c = 0; c < count; ++c)
        {
            const uint32_t bit = 1u << c;
            if ((lastMask & bit) != 0u && (mask & bit) == 0u)
            {
                e.noteOff (c);
                sustained[static_cast<size_t> (c)] = false;
            }
        }
        lastMask = mask;
    }
}

void EngineHost::silenceAll() noexcept
{
    auto& e = activeEngine();
    for (int c = 0; c < e.numChannels(); ++c)
        e.noteOff (c);
    allocator.allNotesOff();
    sustained.fill (false);
}

void EngineHost::collectMidi (const juce::MidiBuffer& midi, int sliceStart, int numSamples) noexcept
{
    noteIn.clear();
    numControls = 0;

    auto addControl = [this] (ControlEvent::Kind kind, int offset, int channel, int a, int b)
    {
        if (numControls >= kMaxControlEvents)
            return;
        auto& e = controls[static_cast<size_t> (numControls++)];
        e.kind = kind;
        e.offset = offset;
        e.midiChannel = static_cast<uint8_t> (juce::jlimit (1, 16, channel));
        e.a = a;
        e.b = b;
    };

    for (const auto meta : midi)
    {
        const int position = meta.samplePosition - sliceStart;
        if (position < 0 || position >= numSamples)
            continue;
        const auto* d = meta.data;
        const int n = meta.numBytes;
        if (n < 2 || d == nullptr)
            continue;

        const int status = d[0] & 0xF0;
        const int channel = (d[0] & 0x0F) + 1;
        const int data1 = d[1] & 0x7F;
        const int data2 = n >= 3 ? (d[2] & 0x7F) : 0;

        switch (status)
        {
            case 0x90:
                if (n >= 3 && data2 > 0)
                    noteIn.push (chipdsp::NoteEvent::on (data1, static_cast<float> (data2) / 127.0f, channel, position));
                else
                    noteIn.push (chipdsp::NoteEvent::off (data1, channel, position));
                break;
            case 0x80:
                noteIn.push (chipdsp::NoteEvent::off (data1, channel, position));
                break;
            case 0xE0:
                if (n >= 3)
                    addControl (ControlEvent::Kind::PitchBend, position, channel, data1 | (data2 << 7), 0);
                break;
            case 0xB0:
                if (n < 3)
                    break;
                if (data1 == 64)
                    addControl (data2 >= 64 ? ControlEvent::Kind::SustainOn : ControlEvent::Kind::SustainOff, position, channel, data1, data2);
                else if (data1 == 120 || data1 == 123)
                    addControl (ControlEvent::Kind::AllNotesOff, position, channel, data1, data2);
                else if (data1 == 121)
                    addControl (ControlEvent::Kind::ResetControllers, position, channel, data1, data2);
                else
                    addControl (ControlEvent::Kind::Controller, position, channel, data1, data2);
                break;
            default:
                break;
        }
    }
}

void EngineHost::applyControl (const ControlEvent& e) noexcept
{
    const auto mc = static_cast<size_t> (e.midiChannel - 1);
    switch (e.kind)
    {
        case ControlEvent::Kind::PitchBend:
            bend[mc] = static_cast<float> (e.a - 8192) / 8192.0f * kBendRangeSemitones;
            break;
        case ControlEvent::Kind::SustainOn:
            sustain[mc] = true;
            break;
        case ControlEvent::Kind::SustainOff:
            sustain[mc] = false;
            releaseSustained (e.midiChannel);
            break;
        case ControlEvent::Kind::Controller:
            if (midiLearn != nullptr)
                midiLearn->handleController (e.a, e.b);
            break;
        case ControlEvent::Kind::AllNotesOff:
            silenceAll();
            arp.reset();
            break;
        case ControlEvent::Kind::ResetControllers:
            bend.fill (0.0f);
            for (int ch = 1; ch <= 16; ++ch)
            {
                sustain[static_cast<size_t> (ch - 1)] = false;
                releaseSustained (ch);
            }
            break;
    }
}

void EngineHost::releaseSustained (int midiChannel) noexcept
{
    auto& e = activeEngine();
    for (int c = 0; c < e.numChannels(); ++c)
    {
        const auto i = static_cast<size_t> (c);
        if (sustained[i] && allocator.channelMidiChannel (c) == midiChannel && ! allocator.isChannelHeld (c))
        {
            e.noteOff (c);
            sustained[i] = false;
        }
    }
}

void EngineHost::applyNote (const chipdsp::NoteEvent& e) noexcept
{
    auto& eng = activeEngine();
    const int midiChannel = e.midiChannel;

    if (e.isNoteOn())
    {
        const int ch = allocator.noteOn (e.note, e.velocity, e.sampleOffset, midiChannel);
        if (ch < 0 || ch >= eng.numChannels())
            return;
        const auto i = static_cast<size_t> (ch);
        sustained[i] = false;
        glider.setTarget (ch, static_cast<float> (e.note), allocator.lastNoteOnWasLegato (ch));
        const float pitch = glider.currentNote (ch) + bend[static_cast<size_t> (juce::jlimit (1, 16, midiChannel) - 1)];
        eng.noteOn (ch, pitch, e.velocity);
        lastPitch[i] = pitch;
        pitchValid[i] = true;
        return;
    }

    const int ch = allocator.noteOff (e.note, midiChannel);
    if (ch < 0 || ch >= eng.numChannels())
        return;
    if (sustain[static_cast<size_t> (juce::jlimit (1, 16, midiChannel) - 1)])
        sustained[static_cast<size_t> (ch)] = true;
    else
        eng.noteOff (ch);
}

bool EngineHost::anyGlideActive() const noexcept
{
    const int count = activeEngine().numChannels();
    for (int c = 0; c < count; ++c)
        if (glider.isGliding (c))
            return true;
    return false;
}

void EngineHost::updatePitches() noexcept
{
    auto& e = activeEngine();
    const int count = e.numChannels();
    for (int c = 0; c < count; ++c)
    {
        const auto i = static_cast<size_t> (c);
        if (! pitchValid[i] || allocator.channelNote (c) < 0)
            continue;
        const int mc = allocator.channelMidiChannel (c);
        const float b = (mc >= 1 && mc <= 16) ? bend[static_cast<size_t> (mc - 1)] : 0.0f;
        const float pitch = glider.currentNote (c) + b;
        if (pitch != lastPitch[i])
        {
            e.setChannelPitch (c, pitch);
            lastPitch[i] = pitch;
        }
    }
}

void EngineHost::renderSubBlock (int offset, int len, float* outL, float* outR, bool wantChannels) noexcept
{
    auto& in = activeEngine();
    const int inCount = in.numChannels();

    float* const* chL = nullptr;
    float* const* chR = nullptr;
    if (wantChannels)
    {
        for (int c = 0; c < kMaxChannels; ++c)
        {
            const auto i = static_cast<size_t> (c);
            chanPtrL[i] = chanBlockL[i].data() + offset;
            chanPtrR[i] = chanBlockR[i].data() + offset;
        }
        chL = chanPtrL.data();
        chR = chanPtrR.data();
    }

    in.renderBlock (outL + offset, outR + offset, chL, chR, len);

    if (! fading)
        return;

    auto& out = engine (outgoing);
    const int outCount = out.numChannels();
    float* const* fL = nullptr;
    float* const* fR = nullptr;
    if (wantChannels)
    {
        for (int c = 0; c < kMaxChannels; ++c)
        {
            const auto i = static_cast<size_t> (c);
            fadePtrL[i] = fadeChanL[i].data() + offset;
            fadePtrR[i] = fadeChanR[i].data() + offset;
        }
        fL = fadePtrL.data();
        fR = fadePtrR.data();
    }

    out.renderBlock (fadeMainL.data() + offset, fadeMainR.data() + offset, fL, fR, len);

    for (int i = 0; i < len; ++i)
    {
        const auto t = static_cast<size_t> (std::min (fadePos + i, fadeLength - 1));
        const float gi = fadeIn[t];
        const float go = fadeOut[t];
        const int s = offset + i;
        outL[s] = outL[s] * gi + fadeMainL[static_cast<size_t> (s)] * go;
        outR[s] = outR[s] * gi + fadeMainR[static_cast<size_t> (s)] * go;

        if (wantChannels)
        {
            for (int c = 0; c < channelsThisBlock; ++c)
            {
                const auto ci = static_cast<size_t> (c);
                const float inL = c < inCount ? chanBlockL[ci][static_cast<size_t> (s)] : 0.0f;
                const float inR = c < inCount ? chanBlockR[ci][static_cast<size_t> (s)] : 0.0f;
                const float oL = c < outCount ? fadeChanL[ci][static_cast<size_t> (s)] : 0.0f;
                const float oR = c < outCount ? fadeChanR[ci][static_cast<size_t> (s)] : 0.0f;
                chanBlockL[ci][static_cast<size_t> (s)] = inL * gi + oL * go;
                chanBlockR[ci][static_cast<size_t> (s)] = inR * gi + oR * go;
            }
        }
    }

    fadePos += len;
    if (fadePos >= fadeLength)
    {
        out.reset();
        fading = false;
        crossfading.store (false, std::memory_order_relaxed);
    }
}

void EngineHost::applyMasterGain (float* outL, float* outR, int numSamples, bool wantChannels) noexcept
{
    const float startGain = gain;
    const float endGain = targetGain;
    if (startGain == endGain && endGain == 1.0f)
        return;

    const float step = (endGain - startGain) / static_cast<float> (numSamples);
    float g = startGain;
    for (int i = 0; i < numSamples; ++i)
    {
        g += step;
        outL[i] *= g;
        outR[i] *= g;
    }

    if (wantChannels)
    {
        for (int c = 0; c < channelsThisBlock; ++c)
        {
            const auto ci = static_cast<size_t> (c);
            float* l = chanBlockL[ci].data();
            float* r = chanBlockR[ci].data();
            g = startGain;
            for (int i = 0; i < numSamples; ++i)
            {
                g += step;
                l[i] *= g;
                r[i] *= g;
            }
        }
    }
    gain = endGain;
}

void EngineHost::deliverBuses (const BusMap& buses, int numSamples, bool wantChannels) noexcept
{
    for (int c = 0; c < kMaxChannels; ++c)
    {
        const auto ci = static_cast<size_t> (c);
        float* l = buses.left[ci];
        float* r = buses.right[ci];
        if (l == nullptr)
            continue;
        if (wantChannels && c < channelsThisBlock)
        {
            std::copy_n (chanBlockL[ci].data(), numSamples, l);
            if (r != nullptr)
                std::copy_n (chanBlockR[ci].data(), numSamples, r);
        }
        else
        {
            std::fill_n (l, numSamples, 0.0f);
            if (r != nullptr)
                std::fill_n (r, numSamples, 0.0f);
        }
    }
}

void EngineHost::feedVisualizer (const float* outL, const float* outR, int numSamples, bool wantChannels) noexcept
{
    if (! scopes.isEnabled())
        return;
    scopes.push (VisualizerBuffers::kMainSlot, outL, outR, numSamples);
    if (! wantChannels)
        return;
    for (int c = 0; c < channelsThisBlock; ++c)
    {
        const auto ci = static_cast<size_t> (c);
        scopes.push (VisualizerBuffers::channelSlot (c), chanBlockL[ci].data(), chanBlockR[ci].data(), numSamples);
    }
}

} // namespace rcv
