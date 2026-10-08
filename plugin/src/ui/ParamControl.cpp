#include "ui/ParamControl.h"

#include "ui/Knob.h"
#include "ui/ParamChoice.h"
#include "ui/ParamToggle.h"
#include "ui/Theme.h"

#include <cmath>

namespace rcv
{

bool isToggleParam (const ParamInfo& info)
{
    if (info.kind == ParamKind::Bool)
        return true;
    if (! info.desc.isInteger || info.desc.minValue != 0.0f || info.desc.maxValue != 1.0f)
        return false;
    if (info.desc.choiceLabels == nullptr)
        return true;
    return juce::String (info.desc.choiceLabels[0]) == "Off" && juce::String (info.desc.choiceLabels[1]) == "On";
}

std::unique_ptr<ParamControl> createParamControl (UiContext& ctx, const ParamInfo& info,
                                                  const juce::String& label, ControlStyle style)
{
    if (isToggleParam (info))
        return std::make_unique<ParamToggle> (ctx, info, label);
    if (info.kind == ParamKind::Choice)
        return std::make_unique<ParamChoice> (ctx, info, label, style);
    return std::make_unique<Knob> (ctx, info, label, style);
}

// ----- ParamControl ------------------------------------------------------------------------

ParamControl::ParamControl (UiContext* context, const ParamInfo* paramInfo, juce::String labelIn, ControlStyle styleIn)
    : ctx (context), info (paramInfo), label (std::move (labelIn)), controlStyle (styleIn)
{
    // A click on the control (or on its inner widget, which never takes focus) focuses the
    // control itself: it handles no keys, so keys go on to the host, and focus never jumps
    // to another control of the group.
    setWantsKeyboardFocus (true);
}

int ParamControl::labelWidth() const
{
    if (fixedLabelWidth >= 0)
        return fixedLabelWidth;
    return static_cast<int> (std::ceil (theme::textWidth (theme::layoutNameFont(), label.toUpperCase())));
}

int ParamControl::stackedLabelWidth() const
{
    return static_cast<int> (std::ceil (theme::textWidth (theme::layoutNameFont(), label.toUpperCase()) * theme::kNameLayoutScale));
}

bool ParamControl::isLearning() const
{
    return ctx != nullptr && info != nullptr && ctx->midiLearn.isLearning()
           && ctx->midiLearn.learningParamId() == info->id;
}

juce::String ParamControl::tooltipText() const
{
    if (info == nullptr)
        return label;
    juce::String text (info->name);
    if (const auto value = tooltipValue(); value.isNotEmpty())
        text << ": " << value;
    if (ctx != nullptr)
    {
        const int cc = ctx->midiLearn.controllerForParam (info->id);
        if (cc >= 0)
            text << " (MIDI CC " << cc << ")";
    }
    return text;
}

void ParamControl::paintOverChildren (juce::Graphics& g)
{
    if (! isLearning())
        return;
    g.setColour (theme::colours::listAmber);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (theme::kBorder * 0.5f), theme::kRadius, theme::kBorder);
}

void ParamControl::showMidiLearnMenu()
{
    if (ctx == nullptr || info == nullptr)
        return;

    auto& learn = ctx->midiLearn;
    const int cc = learn.controllerForParam (info->id);
    const bool learningThis = isLearning();

    juce::PopupMenu menu;
    menu.addSectionHeader (info->name);
    if (learningThis)
        menu.addItem (3, "Cancel MIDI learn");
    else
        menu.addItem (1, cc >= 0 ? "MIDI learn (now CC " + juce::String (cc) + ")" : juce::String ("MIDI learn"));
    if (cc >= 0)
        menu.addItem (2, "Clear MIDI mapping (CC " + juce::String (cc) + ")");

    menu.setLookAndFeel (&getLookAndFeel());   // otherwise the menu uses the default LookAndFeel

    const juce::String id = info->id;
    juce::Component::SafePointer<ParamControl> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [safeThis, id] (int result)
                        {
                            if (safeThis == nullptr || safeThis->ctx == nullptr)
                                return;
                            auto& ml = safeThis->ctx->midiLearn;
                            if (result == 1)
                                ml.learn (id);
                            else if (result == 2)
                                ml.clearMapping (id);
                            else if (result == 3)
                                ml.cancelLearn();
                        });
}

} // namespace rcv
