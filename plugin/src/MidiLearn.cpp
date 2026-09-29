#include "MidiLearn.h"

namespace rcv
{

namespace
{
    const juce::Identifier kTreeType ("MidiLearn");
    const juce::Identifier kMapType ("Map");
    const juce::Identifier kCcProp ("cc");
    const juce::Identifier kParamProp ("param");
} // namespace

MidiLearn::MidiLearn (juce::AudioProcessorValueTreeState& state)
    : apvts (state),
      latestValue (static_cast<size_t> (state.processor.getParameters().size())),
      dirty (static_cast<size_t> (state.processor.getParameters().size()))
{
    for (auto& slot : ccToParam)
        slot.store (-1, std::memory_order_relaxed);
    for (auto& d : dirty)
        d.store (false, std::memory_order_relaxed);
    for (auto& v : latestValue)
        v.store (0.0f, std::memory_order_relaxed);

    const auto& list = apvts.processor.getParameters();
    params.resize (static_cast<size_t> (list.size()), nullptr);
    for (auto* p : list)
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
            if (juce::isPositiveAndBelow (p->getParameterIndex(), list.size()))
                params[static_cast<size_t> (p->getParameterIndex())] = withId;

    startTimer (kDrainIntervalMs);
}

MidiLearn::~MidiLearn()
{
    stopTimer();
}

// ----- message thread ------------------------------------------------------------------------

int MidiLearn::indexForId (const juce::String& paramId) const
{
    for (size_t i = 0; i < params.size(); ++i)
        if (params[i] != nullptr && params[i]->paramID == paramId)
            return static_cast<int> (i);
    return -1;
}

juce::String MidiLearn::idForIndex (int index) const
{
    if (juce::isPositiveAndBelow (index, static_cast<int> (params.size())) && params[static_cast<size_t> (index)] != nullptr)
        return params[static_cast<size_t> (index)]->paramID;
    return {};
}

void MidiLearn::learn (const juce::String& paramId)
{
    drain();   // complete a learn the audio thread may have finished since the last drain
    const int index = indexForId (paramId);
    armedParam.store (index, std::memory_order_relaxed);
    sendChangeMessage();
}

void MidiLearn::cancelLearn()
{
    armedParam.store (-1, std::memory_order_relaxed);
    sendChangeMessage();
}

juce::String MidiLearn::learningParamId() const
{
    return idForIndex (armedParam.load (std::memory_order_relaxed));
}

void MidiLearn::setMapping (int cc, const juce::String& paramId)
{
    if (! juce::isPositiveAndBelow (cc, kNumControllers))
        return;
    const int index = indexForId (paramId);
    if (index < 0)
        return;
    bind (cc, index);
    sendChangeMessage();
}

void MidiLearn::bind (int cc, int paramIndex)
{
    // One CC per parameter: forget any other CC bound to this parameter.
    for (int other = 0; other < kNumControllers; ++other)
        if (other != cc && ccToParam[static_cast<size_t> (other)].load (std::memory_order_relaxed) == paramIndex)
            ccToParam[static_cast<size_t> (other)].store (-1, std::memory_order_relaxed);
    ccToParam[static_cast<size_t> (cc)].store (paramIndex, std::memory_order_relaxed);
}

void MidiLearn::clearMapping (const juce::String& paramId)
{
    const int index = indexForId (paramId);
    if (index < 0)
        return;
    for (auto& slot : ccToParam)
        if (slot.load (std::memory_order_relaxed) == index)
            slot.store (-1, std::memory_order_relaxed);
    sendChangeMessage();
}

void MidiLearn::clearController (int cc)
{
    if (! juce::isPositiveAndBelow (cc, kNumControllers))
        return;
    ccToParam[static_cast<size_t> (cc)].store (-1, std::memory_order_relaxed);
    sendChangeMessage();
}

void MidiLearn::clearAll()
{
    for (auto& slot : ccToParam)
        slot.store (-1, std::memory_order_relaxed);
    armedParam.store (-1, std::memory_order_relaxed);
    pendingLearn.store (-1, std::memory_order_relaxed);
    sendChangeMessage();
}

int MidiLearn::controllerForParam (const juce::String& paramId) const
{
    const int index = indexForId (paramId);
    if (index < 0)
        return -1;
    for (int cc = 0; cc < kNumControllers; ++cc)
        if (ccToParam[static_cast<size_t> (cc)].load (std::memory_order_relaxed) == index)
            return cc;
    return -1;
}

juce::String MidiLearn::paramForController (int cc) const
{
    if (! juce::isPositiveAndBelow (cc, kNumControllers))
        return {};
    return idForIndex (ccToParam[static_cast<size_t> (cc)].load (std::memory_order_relaxed));
}

juce::ValueTree MidiLearn::toValueTree() const
{
    juce::ValueTree tree (kTreeType);
    for (int cc = 0; cc < kNumControllers; ++cc)
    {
        const auto id = idForIndex (ccToParam[static_cast<size_t> (cc)].load (std::memory_order_relaxed));
        if (id.isEmpty())
            continue;
        juce::ValueTree map (kMapType);
        map.setProperty (kCcProp, cc, nullptr);
        map.setProperty (kParamProp, id, nullptr);
        tree.addChild (map, -1, nullptr);
    }
    return tree;
}

void MidiLearn::restoreFromValueTree (const juce::ValueTree& tree)
{
    for (auto& slot : ccToParam)
        slot.store (-1, std::memory_order_relaxed);
    armedParam.store (-1, std::memory_order_relaxed);
    pendingLearn.store (-1, std::memory_order_relaxed);

    if (tree.isValid() && tree.hasType (kTreeType))
    {
        for (const auto& map : tree)
        {
            if (! map.hasType (kMapType))
                continue;
            const int cc = static_cast<int> (map.getProperty (kCcProp, -1));
            const int index = indexForId (map.getProperty (kParamProp).toString());
            if (juce::isPositiveAndBelow (cc, kNumControllers) && index >= 0)
                ccToParam[static_cast<size_t> (cc)].store (index, std::memory_order_relaxed);
        }
    }
    sendChangeMessage();
}

void MidiLearn::drain()
{
    // Learn completion: the reassignment (one CC per parameter) and the change message.
    const int learned = pendingLearn.exchange (-1, std::memory_order_acquire);
    if (learned >= 0)
    {
        const int cc = learned & 0xFF;
        const int index = learned >> 8;
        if (juce::isPositiveAndBelow (cc, kNumControllers) && juce::isPositiveAndBelow (index, static_cast<int> (params.size())))
            bind (cc, index);
        sendChangeMessage();
    }

    if (! anyDirty.exchange (false, std::memory_order_acquire))
        return;

    for (size_t i = 0; i < params.size(); ++i)
    {
        if (! dirty[i].exchange (false, std::memory_order_acquire))
            continue;
        auto* param = params[i];
        if (param == nullptr)
            continue;
        const float value = juce::jlimit (0.0f, 1.0f, latestValue[i].load (std::memory_order_relaxed));
        param->beginChangeGesture();
        param->setValueNotifyingHost (value);
        param->endChangeGesture();
    }
}

// ----- audio thread --------------------------------------------------------------------------

void MidiLearn::handleController (int cc, int value) noexcept
{
    if (! juce::isPositiveAndBelow (cc, kNumControllers))
        return;

    const int armed = armedParam.exchange (-1, std::memory_order_relaxed);
    if (armed >= 0)
    {
        // Learn: bind this CC to the armed parameter now so the value below reaches it; the
        // message thread completes the reassignment and notifies. pendingLearn is a single
        // atomic, so a completed learn can never be lost.
        ccToParam[static_cast<size_t> (cc)].store (armed, std::memory_order_relaxed);
        pendingLearn.store ((armed << 8) | cc, std::memory_order_release);
    }

    const int target = ccToParam[static_cast<size_t> (cc)].load (std::memory_order_relaxed);
    if (! juce::isPositiveAndBelow (target, static_cast<int> (latestValue.size())))
        return;

    const auto i = static_cast<size_t> (target);
    latestValue[i].store (static_cast<float> (juce::jlimit (0, 127, value)) / 127.0f, std::memory_order_relaxed);
    dirty[i].store (true, std::memory_order_release);
    anyDirty.store (true, std::memory_order_release);
}

} // namespace rcv
