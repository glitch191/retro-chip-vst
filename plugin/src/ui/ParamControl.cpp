#include "ui/ParamControl.h"

#include "ui/Knob.h"
#include "ui/ParamChoice.h"
#include "ui/ParamToggle.h"
#include "ui/Theme.h"

#include <cmath>

namespace rcv
{

juce::String formatNativeValue (const ParamInfo& info, float native)
{
    const auto& d = info.desc;
    if (d.choiceLabels != nullptr)
    {
        const int count = static_cast<int> (std::lround (d.maxValue - d.minValue)) + 1;
        const int index = juce::jlimit (0, count - 1, static_cast<int> (std::lround (native - d.minValue)));
        return juce::String (d.choiceLabels[index]);
    }
    if (info.kind == ParamKind::Bool)
        return native > 0.5f ? "On" : "Off";

    // Hardware units that read better converted (keyed by the ParamDesc unit string; the
    // engine value and range stay the register value).
    const juce::String unit (d.unit != nullptr ? d.unit : "");
    const int steps = static_cast<int> (std::lround (native));
    if (unit == "x2 dB")   // SN76489 attenuation: 2 dB per step, 15 = off
        return steps >= 15 ? juce::String ("Off") : (steps <= 0 ? juce::String ("0 dB") : "-" + juce::String (2 * steps) + " dB");
    if (unit == "TL")      // YM2612 total level: 0.75 dB per step
        return steps <= 0 ? juce::String ("0 dB") : "-" + juce::String (0.75 * steps, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " dB";

    juce::String text;
    if (d.isInteger)
    {
        text = juce::String (steps);
    }
    else
    {
        const float range = d.maxValue - d.minValue;
        const int decimals = (range >= 100.0f || unit == "%") ? 0 : (range >= 10.0f ? 1 : 2);
        text = juce::String (native, decimals);
    }
    if (d.minValue < 0.0f && native > 0.0f && unit.isNotEmpty())   // signed offsets: "+12 st", "+5 cents"
        text = "+" + text;

    if (unit.isEmpty() || unit == "x")
        return text;
    // Register steps (YM2612 F-number, SN76489 tone period): "3 steps" reads better than "3 fnum".
    if (unit == "fnum" || unit == "period")
        return text + (std::abs (steps) == 1 ? " step" : " steps");
    // Counted units read in the singular for one: "1 frame", "1 tick", "1 cent".
    if (d.isInteger && std::abs (steps) == 1 && (unit == "frames" || unit == "ticks" || unit == "cents"))
        return text + " " + unit.dropLastCharacters (1);
    return text + " " + unit;
}

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
    return static_cast<int> (std::ceil (theme::textWidth (theme::font(), label)));
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
    g.setColour (theme::colours::accent);
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
