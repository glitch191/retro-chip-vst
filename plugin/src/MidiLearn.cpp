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

MidiLearn::MidiLearn (juce::AudioProcessorValueTreeState& state) : apvts (state)
{
    for (auto& slot : ccToParam)
        slot.store (-1, std::memory_order_relaxed);

    const auto& list = apvts.processor.getParameters();
    params.resize (static_cast<size_t> (list.size()), nullptr);
    for (auto* p : list)
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
            if (juce::isPositiveAndBelow (p->getParameterIndex(), list.size()))
                params[static_cast<size_t> (p->getParameterIndex())] = withId;
}

MidiLearn::~MidiLearn()
{
    cancelPendingUpdate();
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

    // One CC per parameter: forget any other CC bound to this parameter.
    for (auto& slot : ccToParam)
        if (slot.load (std::memory_order_relaxed) == index)
            slot.store (-1, std::memory_order_relaxed);

    ccToParam[static_cast<size_t> (cc)].store (index, std::memory_order_relaxed);
    sendChangeMessage();
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
    bool mapChanged = false;
    while (fifo.getNumReady() > 0)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (fifo.getNumReady(), start1, size1, start2, size2);

        auto apply = [this, &mapChanged] (const Event& e)
        {
            if (e.type == Event::Type::Learned)
            {
                if (juce::isPositiveAndBelow (e.cc, kNumControllers) && e.paramIndex >= 0)
                {
                    // Reassign: a CC bound elsewhere moves here; a parameter keeps one CC.
                    for (auto& slot : ccToParam)
                        if (slot.load (std::memory_order_relaxed) == e.paramIndex)
                            slot.store (-1, std::memory_order_relaxed);
                    ccToParam[static_cast<size_t> (e.cc)].store (e.paramIndex, std::memory_order_relaxed);
                    mapChanged = true;
                }
                return;
            }

            if (! juce::isPositiveAndBelow (e.paramIndex, static_cast<int> (params.size())))
                return;
            auto* param = params[static_cast<size_t> (e.paramIndex)];
            if (param == nullptr)
                return;
            param->beginChangeGesture();
            param->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, e.normalised));
            param->endChangeGesture();
        };

        for (int i = 0; i < size1; ++i)
            apply (events[static_cast<size_t> (start1 + i)]);
        for (int i = 0; i < size2; ++i)
            apply (events[static_cast<size_t> (start2 + i)]);

        fifo.finishedRead (size1 + size2);
    }

    if (mapChanged)
        sendChangeMessage();
}

// ----- audio thread --------------------------------------------------------------------------

void MidiLearn::handleController (int cc, int value) noexcept
{
    if (! juce::isPositiveAndBelow (cc, kNumControllers))
        return;

    Event e;
    const int armed = armedParam.load (std::memory_order_relaxed);
    if (armed >= 0)
    {
        // Learn: bind this CC to the armed parameter (the map itself is updated on the
        // message thread so listeners are notified there), then apply the value as well.
        armedParam.store (-1, std::memory_order_relaxed);
        ccToParam[static_cast<size_t> (cc)].store (armed, std::memory_order_relaxed);
        e.type = Event::Type::Learned;
        e.paramIndex = armed;
        e.cc = cc;
        if (fifo.getFreeSpace() > 0)
        {
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            fifo.prepareToWrite (1, start1, size1, start2, size2);
            if (size1 > 0)
                events[static_cast<size_t> (start1)] = e;
            fifo.finishedWrite (size1 + size2);
        }
    }

    const int target = ccToParam[static_cast<size_t> (cc)].load (std::memory_order_relaxed);
    if (target < 0)
        return;

    e.type = Event::Type::Value;
    e.paramIndex = target;
    e.cc = cc;
    e.normalised = static_cast<float> (juce::jlimit (0, 127, value)) / 127.0f;

    if (fifo.getFreeSpace() > 0)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (1, start1, size1, start2, size2);
        if (size1 > 0)
            events[static_cast<size_t> (start1)] = e;
        fifo.finishedWrite (size1 + size2);
    }

    triggerAsyncUpdate();
}

} // namespace rcv
