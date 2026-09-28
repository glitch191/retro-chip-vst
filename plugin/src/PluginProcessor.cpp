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
}

RetroChipProcessor::~RetroChipProcessor() = default;

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
    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
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

    auto mainBus = getBusBuffer (buffer, false, 0);
    host.process (mainBus, midi, transport, buses);
}

juce::AudioProcessorEditor* RetroChipProcessor::createEditor()
{
    return new RetroChipEditor (*this);
}

// ----- state ----------------------------------------------------------------------------------

void RetroChipProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root (kStateType);
    root.setProperty (kVersionProp, kStateVersion, nullptr);
    root.setProperty (kPresetNameProp, presets.currentName(), nullptr);
    root.setProperty (kPresetCategoryProp, presets.currentCategory(), nullptr);
    root.addChild (apvts.copyState(), -1, nullptr);
    root.addChild (learn.toValueTree(), -1, nullptr);
    root.addChild (presets.userSamplesToValueTree(), -1, nullptr);

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

    const auto presetName = root.getProperty (kPresetNameProp).toString();
    const auto presetCategory = root.getProperty (kPresetCategoryProp).toString();
    const auto samples = root.getChildWithName (kUserSamplesType).createCopy();

    // Sample loading talks to the engines' message-thread API.
    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        presets.restoreCurrent (presetName, presetCategory);
        presets.restoreUserSamples (samples);
    }
    else
    {
        juce::MessageManager::callAsync ([this, presetName, presetCategory, samples]
        {
            presets.restoreCurrent (presetName, presetCategory);
            presets.restoreUserSamples (samples);
        });
    }
}

} // namespace rcv

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new rcv::RetroChipProcessor();
}
