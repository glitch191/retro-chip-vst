#include "PluginProcessor.h"

#include "PluginEditor.h"

#include "chipdsp/EngineFactory.h"

namespace rcv
{

namespace
{
    const juce::Identifier kStateType ("RetroChipState");
    const juce::Identifier kParametersType ("Parameters");
    const juce::Identifier kVersionProp ("version");
    const juce::Identifier kPresetNameProp ("presetName");
    const juce::Identifier kPresetCategoryProp ("presetCategory");
    const juce::Identifier kMidiLearnType ("MidiLearn");
    const juce::Identifier kUserSamplesType ("UserSamples");

    std::array<const chipdsp::IChipEngine*, 3> enginesOf (const EngineHost& host)
    {
        return { &host.engine (chipdsp::ChipId::Nes), &host.engine (chipdsp::ChipId::Snes), &host.engine (chipdsp::ChipId::Genesis) };
    }
} // namespace

juce::String RetroChipProcessor::channelBusName (int busIndex)
{
    return "Out " + juce::String (busIndex);
}

juce::AudioProcessor::BusesProperties RetroChipProcessor::makeBuses()
{
    BusesProperties props;
    props = props.withOutput ("Main", juce::AudioChannelSet::stereo(), true);
    for (int i = 1; i <= kNumChannelBuses; ++i)
        props = props.withOutput (channelBusName (i), juce::AudioChannelSet::stereo(), false);
    return props;
}

RetroChipProcessor::RetroChipProcessor()
    : AudioProcessor (makeBuses()),
      host(),
      registry (enginesOf (host)),
      apvts (*this, nullptr, kParametersType, registry.createParameterLayout()),
      learn (apvts),
      presets (apvts, registry, host),
      randomizerImpl (apvts, registry)
{
    host.attachParameters (registry, apvts);
    host.setMidiLearn (&learn);
    presets.loadBanks();

    startTimer (kServiceIntervalMs);
}

RetroChipProcessor::~RetroChipProcessor()
{
    stopTimer();
}

// ----- message-thread services ----------------------------------------------------------------

void RetroChipProcessor::timerCallback()
{
    applyPendingRestore();
}

void RetroChipProcessor::applyPendingRestore()
{
    // The queued state stays visible to getStateInformation() until it has been applied, so
    // a getState that races the restore still writes the restored preset and samples.
    std::optional<PendingRestore> state;
    {
        const juce::ScopedLock sl (pendingLock);
        state = pendingRestore;
    }
    if (! state.has_value())
        return;

    presets.restoreState (state->presetName, state->presetCategory, state->samples);

    const juce::ScopedLock sl (pendingLock);
    if (pendingRestore.has_value() && pendingRestore->serial == state->serial)
        pendingRestore.reset();
}

chipdsp::ChipId RetroChipProcessor::selectedChip() const noexcept
{
    if (auto* raw = apvts.getRawParameterValue (ParamIds::chip))
        return static_cast<chipdsp::ChipId> (juce::jlimit (0, ParamRegistry::kNumChips - 1, static_cast<int> (raw->load (std::memory_order_relaxed))));
    return chipdsp::ChipId::Nes;
}

// ----- lifecycle ------------------------------------------------------------------------------

void RetroChipProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    host.prepare (sampleRate, samplesPerBlock);
    testMidi.ensureSize (2048);
}

void RetroChipProcessor::queueTestNotes (std::initializer_list<int> notes, int velocity)
{
    if (numTestNotes.load (std::memory_order_acquire) != 0)
        return;
    int n = 0;
    for (int note : notes)
        if (n < kMaxTestNotes)
            testNotes[static_cast<size_t> (n++)].store (juce::jlimit (0, 127, note), std::memory_order_relaxed);
    testVelocity.store (juce::jlimit (1, 127, velocity), std::memory_order_relaxed);
    numTestNotes.store (n, std::memory_order_release);
}

void RetroChipProcessor::releaseResources()
{
}

void RetroChipProcessor::reset()
{
    host.reset();
}

bool RetroChipProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    for (const auto& input : layouts.inputBuses)
        if (! input.isDisabled())
            return false;

    if (layouts.outputBuses.isEmpty() || layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    for (int i = 1; i < layouts.outputBuses.size(); ++i)
    {
        const auto& set = layouts.outputBuses.getReference (i);
        if (! set.isDisabled() && set != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void RetroChipProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    buffer.clear();
    if (numSamples <= 0 || ! host.isPrepared())
        return;

    chipdsp::TransportInfo transport;
    if (auto* hostPlayHead = getPlayHead())
    {
        if (const auto position = hostPlayHead->getPosition())
        {
            if (const auto bpm = position->getBpm())
                transport.bpm = *bpm > 0.0 ? *bpm : 120.0;
            if (const auto ppq = position->getPpqPosition())
                transport.ppqPosition = *ppq;
            transport.isPlaying = position->getIsPlaying();
        }
    }

    EngineHost::BusMap buses;
    for (int b = 1; b <= kNumChannelBuses; ++b)
    {
        const auto* bus = getBus (false, b);
        if (bus == nullptr || ! bus->isEnabled())
            continue;
        auto busBuffer = getBusBuffer (buffer, false, b);
        if (busBuffer.getNumChannels() < 2)
            continue;
        buses.left[static_cast<size_t> (b - 1)] = busBuffer.getWritePointer (0);
        buses.right[static_cast<size_t> (b - 1)] = busBuffer.getWritePointer (1);
    }

    // Development hook only (queueTestNotes): the host's events plus the queued note-ons.
    juce::MidiBuffer* events = &midi;
    if (const int n = numTestNotes.load (std::memory_order_acquire); n > 0)
    {
        testMidi.clear();
        testMidi.addEvents (midi, 0, -1, 0);
        const auto velocity = static_cast<juce::uint8> (testVelocity.load (std::memory_order_relaxed));
        for (int i = 0; i < n; ++i)
            testMidi.addEvent (juce::MidiMessage::noteOn (1, testNotes[static_cast<size_t> (i)].load (std::memory_order_relaxed), velocity), 0);
        numTestNotes.store (0, std::memory_order_release);
        events = &testMidi;
    }

    auto mainBus = getBusBuffer (buffer, false, 0);
    host.process (mainBus, *events, transport, buses);
}

juce::AudioProcessorEditor* RetroChipProcessor::createEditor()
{
    return new RetroChipEditor (*this);
}

// ----- state ----------------------------------------------------------------------------------

void RetroChipProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Hosts may call this from any thread: the preset name and the user samples come from
    // the latest setStateInformation() when its restore is still queued, otherwise from the
    // preset manager's thread-safe snapshot.
    PresetManager::StateSnapshot nonParams;
    {
        const juce::ScopedLock sl (pendingLock);
        if (pendingRestore.has_value())
        {
            nonParams.presetName = pendingRestore->presetName;
            nonParams.presetCategory = pendingRestore->presetCategory;
            nonParams.samples = pendingRestore->samples;
        }
        else
        {
            nonParams = presets.stateSnapshot();
        }
    }

    juce::ValueTree root (kStateType);
    root.setProperty (kVersionProp, kStateVersion, nullptr);
    root.setProperty (kPresetNameProp, nonParams.presetName, nullptr);
    root.setProperty (kPresetCategoryProp, nonParams.presetCategory, nullptr);
    root.addChild (apvts.copyState(), -1, nullptr);
    root.addChild (learn.toValueTree(), -1, nullptr);
    root.addChild (nonParams.samples.isValid() ? nonParams.samples.createCopy() : juce::ValueTree (kUserSamplesType), -1, nullptr);

    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

void RetroChipProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;
    const auto root = juce::ValueTree::fromXml (*xml);
    if (! root.hasType (kStateType))
        return;

    const auto params = root.getChildWithName (kParametersType);
    if (params.isValid())
        apvts.replaceState (params);

    learn.restoreFromValueTree (root.getChildWithName (kMidiLearnType));

    PendingRestore state;
    state.presetName = root.getProperty (kPresetNameProp).toString();
    state.presetCategory = root.getProperty (kPresetCategoryProp).toString();
    state.samples = root.getChildWithName (kUserSamplesType).createCopy();
    if (! state.samples.isValid())
        state.samples = juce::ValueTree (kUserSamplesType);
    {
        // A newer state supersedes a queued one.
        const juce::ScopedLock sl (pendingLock);
        state.serial = ++restoreSerial;
        pendingRestore = std::move (state);
    }

    // Sample loading talks to the engines' message-thread API: applied now on the message
    // thread, otherwise by the service timer.
    if (juce::MessageManager::existsAndIsCurrentThread())
        applyPendingRestore();
}

} // namespace rcv

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new rcv::RetroChipProcessor();
}
