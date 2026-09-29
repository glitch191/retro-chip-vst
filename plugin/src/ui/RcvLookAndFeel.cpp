#include "ui/RcvLookAndFeel.h"

#include "ui/ParamControl.h"
#include "ui/Theme.h"

#include <cmath>

namespace rcv
{

namespace
{
    namespace c = theme::colours;

    const juce::Identifier kHoverKey ("rcvTransitionHover");
    const juce::Identifier kStateKey ("rcvTransitionState");

    juce::Point<float> onCircle (juce::Point<float> centre, float radius, float angle) noexcept
    {
        // JUCE rotary angles run clockwise from 12 o'clock.
        return { centre.x + radius * std::sin (angle), centre.y - radius * std::cos (angle) };
    }

    void drawChevron (juce::Graphics& g, juce::Point<float> centre, bool pointsRight, juce::Colour colour)
    {
        constexpr float l = theme::kChevronHalfLength;
        constexpr float d = theme::kChevronHalfDepth;
        juce::Path p;
        if (pointsRight)
        {
            p.startNewSubPath (centre.x - d, centre.y - l);
            p.lineTo (centre.x + d, centre.y);
            p.lineTo (centre.x - d, centre.y + l);
        }
        else
        {
            p.startNewSubPath (centre.x - l, centre.y - d);
            p.lineTo (centre.x, centre.y + d);
            p.lineTo (centre.x + l, centre.y - d);
        }
        g.setColour (colour);
        g.strokePath (p, juce::PathStrokeType (theme::kChevronThickness, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    }
    // juce::CaretComponent blinks through its own juce::Timer, which repaints the text field
    // about three times per second while it has focus. This caret is shown or hidden with the
    // focus and never blinks, so nothing repaints at rest.
    class SteadyCaret final : public juce::CaretComponent
    {
    public:
        explicit SteadyCaret (juce::Component* keyFocusOwner) : juce::CaretComponent (keyFocusOwner), owner (keyFocusOwner) {}

        void setCaretPosition (const juce::Rectangle<int>& characterArea) override
        {
            setVisible (owner == nullptr || (owner->hasKeyboardFocus (false) && ! owner->isCurrentlyBlockedByAnotherModalComponent()));
            setBounds (characterArea.withWidth (theme::kCaretWidth));
        }

    private:
        juce::Component* owner;
    };
} // namespace

RcvLookAndFeel::RcvLookAndFeel()
{
    setColourScheme ({ c::background, c::surface, c::panel, c::border, c::text, c::accent, c::onAccent, c::accent, c::text });

    setColour (juce::ResizableWindow::backgroundColourId, c::background);
    setColour (juce::DocumentWindow::textColourId, c::text);
    setColour (juce::Label::textColourId, c::text);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);

    setColour (juce::TextButton::buttonColourId, c::surface);
    setColour (juce::TextButton::buttonOnColourId, c::accent);
    setColour (juce::TextButton::textColourOffId, c::text);
    setColour (juce::TextButton::textColourOnId, c::onAccent);
    setColour (juce::ToggleButton::textColourId, c::text);

    setColour (juce::ComboBox::backgroundColourId, c::surface);
    setColour (juce::ComboBox::textColourId, c::text);
    setColour (juce::ComboBox::outlineColourId, c::border);
    setColour (juce::ComboBox::arrowColourId, c::textDim);
    setColour (juce::ComboBox::focusedOutlineColourId, c::accent);

    setColour (juce::PopupMenu::backgroundColourId, c::panel);
    setColour (juce::PopupMenu::textColourId, c::text);
    setColour (juce::PopupMenu::headerTextColourId, c::textDim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, c::surfaceHover);
    setColour (juce::PopupMenu::highlightedTextColourId, c::text);

    setColour (juce::TextEditor::backgroundColourId, c::surface);
    setColour (juce::TextEditor::textColourId, c::text);
    setColour (juce::TextEditor::outlineColourId, c::border);
    setColour (juce::TextEditor::focusedOutlineColourId, c::accent);
    setColour (juce::TextEditor::highlightColourId, c::accent.withAlpha (0.35f));
    setColour (juce::TextEditor::highlightedTextColourId, c::text);
    setColour (juce::CaretComponent::caretColourId, c::text);

    setColour (juce::ListBox::backgroundColourId, c::panel);
    setColour (juce::ListBox::outlineColourId, c::border);
    setColour (juce::ListBox::textColourId, c::text);
    setColour (juce::ScrollBar::thumbColourId, c::track);

    setColour (juce::TooltipWindow::backgroundColourId, c::panel);
    setColour (juce::TooltipWindow::textColourId, c::text);
    setColour (juce::TooltipWindow::outlineColourId, c::border);

    setColour (juce::Slider::rotarySliderFillColourId, c::accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, c::track);
    setColour (juce::Slider::thumbColourId, c::text);

    setDefaultSansSerifTypefaceName (theme::fontFamily());
}

// ----- transitions --------------------------------------------------------------------------

float RcvLookAndFeel::valueAt (const Transition& t, double nowMs) noexcept
{
    const double p = juce::jlimit (0.0, 1.0, (nowMs - t.startMs) / theme::kTransitionMs);
    const float eased = static_cast<float> (1.0 - (1.0 - p) * (1.0 - p));   // ease-out
    return t.from + (t.to - t.from) * eased;
}

float RcvLookAndFeel::transition (juce::Component& component, Channel channel, bool target)
{
    const float to = target ? 1.0f : 0.0f;
    const double now = juce::Time::getMillisecondCounterHiRes();
    const auto& key = channel == hoverChannel ? kHoverKey : kStateKey;
    auto& props = component.getProperties();

    for (auto& t : running)
    {
        if (t.component.getComponent() != &component || t.channel != channel)
            continue;
        const float value = valueAt (t, now);
        if ((t.to > 0.5f) != target)
        {
            t.from = value;
            t.to = to;
            t.startMs = now;
            props.set (key, to);
        }
        return value;
    }

    if (! props.contains (key))
    {
        props.set (key, to);   // first paint: no transition
        return to;
    }
    const float last = static_cast<float> (props[key]);
    if ((last > 0.5f) == target)
        return to;

    props.set (key, to);
    running.push_back ({ &component, channel, last, to, now });
    return last;
}

bool RcvLookAndFeel::updateTransitions()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    for (auto it = running.begin(); it != running.end();)
    {
        auto* comp = it->component.getComponent();
        if (comp != nullptr)
            comp->repaint();
        if (comp == nullptr || now - it->startMs >= theme::kTransitionMs)
            it = running.erase (it);
        else
            ++it;
    }
    return ! running.empty();
}

// ----- rotary slider ------------------------------------------------------------------------

juce::Slider::SliderLayout RcvLookAndFeel::getSliderLayout (juce::Slider& slider)
{
    if (auto* cell = dynamic_cast<CellSlider*> (&slider))
    {
        juce::Slider::SliderLayout layout;
        layout.sliderBounds = cell->knobArea;
        return layout;
    }
    return LookAndFeel_V4::getSliderLayout (slider);
}

void RcvLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                       float startAngle, float endAngle, juce::Slider& slider)
{
    const bool enabled = slider.isEnabled();
    const float hover = transition (slider, hoverChannel, enabled && slider.isMouseOverOrDragging());

    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const float radius = 0.5f * juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    constexpr float arcThickness = theme::kKnobArcThickness;
    const float arcRadius = radius - 0.5f * arcThickness;

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (c::track);
    g.strokePath (track, juce::PathStrokeType (arcThickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));

    // Bipolar ranges (-24..24, -128..127) fill from zero, the others from the minimum.
    float origin = 0.0f;
    if (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0)
        origin = static_cast<float> (slider.valueToProportionOfLength (0.0));
    const float originAngle = startAngle + origin * (endAngle - startAngle);
    const float valueAngle = startAngle + sliderPos * (endAngle - startAngle);
    if (std::abs (valueAngle - originAngle) > 0.001f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                             juce::jmin (originAngle, valueAngle), juce::jmax (originAngle, valueAngle), true);
        g.setColour (enabled ? c::accent : c::textDisabled);
        g.strokePath (value, juce::PathStrokeType (arcThickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }

    const float discRadius = radius - arcThickness - theme::kKnobDiscInset;
    const auto disc = juce::Rectangle<float> (2.0f * discRadius, 2.0f * discRadius).withCentre (centre);
    g.setColour (c::surface.interpolatedWith (c::surfaceHover, hover));
    g.fillEllipse (disc);
    g.setColour (c::border);
    g.drawEllipse (disc, theme::kBorder);

    g.setColour (enabled ? c::text : c::textDisabled);
    g.drawLine (juce::Line<float> (onCircle (centre, discRadius * 0.2f, valueAngle), onCircle (centre, discRadius * 0.9f, valueAngle)), theme::kKnobPointerThickness);
}

// ----- buttons ------------------------------------------------------------------------------

void RcvLookAndFeel::drawFlatButton (juce::Graphics& g, juce::Button& button, const juce::String& text,
                                     bool isHighlighted, bool isDown)
{
    const bool enabled = button.isEnabled();
    const float hover = transition (button, hoverChannel, enabled && (isHighlighted || isDown));
    const float on = transition (button, stateChannel, button.getToggleState());

    const auto b = button.getLocalBounds().toFloat().reduced (0.5f * theme::kBorder);
    auto body = c::surface.interpolatedWith (c::surfaceHover, hover);
    auto fill = body.interpolatedWith (enabled ? c::accent : c::textDisabled, on);
    g.setColour (fill);
    g.fillRoundedRectangle (b, theme::kRadius);
    g.setColour (on > 0.5f ? fill : c::border);
    g.drawRoundedRectangle (b, theme::kRadius, theme::kBorder);

    g.setFont (theme::font());
    g.setColour (on > 0.5f ? c::onAccent : (enabled ? c::text : c::textDisabled));

    // A button that opens a menu (the preset name) reads like a combo box.
    if (static_cast<bool> (button.getProperties()["rcvDropDown"]))
    {
        auto area = button.getLocalBounds();
        const auto arrow = area.removeFromRight (theme::kComboArrowWidth + theme::kOpticalOffset);
        g.drawFittedText (text, area.withTrimmedLeft (theme::kGap), juce::Justification::centredLeft, 1, 1.0f);
        drawChevron (g, arrow.toFloat().getCentre(), false, enabled ? c::textDim : c::textDisabled);
        return;
    }
    g.drawFittedText (text, button.getLocalBounds().reduced (theme::kUnit, 0), juce::Justification::centred, 1, 1.0f);
}

void RcvLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                           bool isHighlighted, bool isDown)
{
    drawFlatButton (g, button, button.getButtonText(), isHighlighted, isDown);
}

void RcvLookAndFeel::drawButtonText (juce::Graphics&, juce::TextButton&, bool, bool)
{
    // drawButtonBackground() already drew the text with the state colour.
}

juce::Font RcvLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return theme::font();
}

void RcvLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool isHighlighted, bool isDown)
{
    drawFlatButton (g, button, button.getButtonText(), isHighlighted, isDown);
}

// ----- combo box ------------------------------------------------------------------------------

void RcvLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const bool enabled = box.isEnabled();
    const float hover = transition (box, hoverChannel, enabled && (box.isMouseOver (true) || box.isPopupActive()));
    const auto b = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f * theme::kBorder);

    g.setColour (c::surface.interpolatedWith (c::surfaceHover, hover));
    g.fillRoundedRectangle (b, theme::kRadius);
    g.setColour (box.hasKeyboardFocus (false) ? c::accent : c::border);
    g.drawRoundedRectangle (b, theme::kRadius, theme::kBorder);

    const juce::Point<float> arrowCentre (static_cast<float> (width) - 0.5f * static_cast<float> (theme::kComboArrowWidth) - static_cast<float> (theme::kOpticalOffset),
                                          0.5f * static_cast<float> (height));
    drawChevron (g, arrowCentre, false, enabled ? c::textDim : c::textDisabled);
}

juce::Font RcvLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return theme::font();
}

void RcvLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBorderSize ({});
    label.setBounds (theme::kGap, 0, juce::jmax (0, box.getWidth() - theme::kGap - theme::kComboArrowWidth), box.getHeight());
    label.setFont (getComboBoxFont (box));
    label.setColour (juce::Label::textColourId, box.isEnabled() ? c::text : c::textDisabled);
}

juce::PopupMenu::Options RcvLookAndFeel::getOptionsForComboBoxPopupMenu (juce::ComboBox& box, juce::Label& label)
{
    return LookAndFeel_V4::getOptionsForComboBoxPopupMenu (box, label).withStandardItemHeight (theme::kPopupItemHeight);
}

// ----- popup menu -----------------------------------------------------------------------------

void RcvLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (c::panel);
    g.setColour (c::border);
    g.drawRect (0, 0, width, height, static_cast<int> (theme::kBorder));
}

void RcvLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                                        bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                                        const juce::String& text, const juce::String& shortcutKeyText,
                                        const juce::Drawable*, const juce::Colour*)
{
    if (isSeparator)
    {
        g.setColour (c::divider);
        g.fillRect (area.reduced (theme::kGap, 0).withSizeKeepingCentre (area.getWidth() - 2 * theme::kGap, 1));
        return;
    }

    auto r = area.reduced (theme::kUnit, 1);
    if (isHighlighted && isActive)
    {
        g.setColour (c::surfaceHover);
        g.fillRoundedRectangle (r.toFloat(), theme::kRadius);
    }
    if (isTicked)
    {
        g.setColour (c::accent);
        g.fillRect (r.withWidth (theme::kTickWidth).reduced (0, theme::kUnit));
    }

    g.setFont (theme::font());
    g.setColour (isActive ? c::text : c::textDisabled);
    auto textArea = r.withTrimmedLeft (theme::kGap + theme::kUnit).withTrimmedRight (hasSubMenu ? theme::kComboArrowWidth : theme::kGap);
    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour (c::textDim);
        g.drawText (shortcutKeyText, textArea, juce::Justification::centredRight, false);
        g.setColour (isActive ? c::text : c::textDisabled);
    }
    g.drawFittedText (text, textArea, juce::Justification::centredLeft, 1, 1.0f);

    if (hasSubMenu)
        drawChevron (g, { static_cast<float> (r.getRight() - theme::kComboArrowWidth / 2), static_cast<float> (r.getCentreY()) }, true,
                     isActive ? c::textDim : c::textDisabled);
}

void RcvLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    g.setFont (theme::font (theme::kFontBody, true));
    g.setColour (c::textDim);
    g.drawFittedText (sectionName, area.withTrimmedLeft (theme::kGap + theme::kUnit).withTrimmedRight (theme::kGap),
                      juce::Justification::centredLeft, 1, 1.0f);
}

void RcvLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                                int& idealWidth, int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth = theme::kPopupSeparatorWidth;
        idealHeight = theme::kPopupSeparatorHeight;
        return;
    }
    idealHeight = juce::jmax (standardMenuItemHeight, theme::kPopupItemHeight);
    idealWidth = static_cast<int> (std::ceil (theme::textWidth (theme::font (theme::kFontBody, true), text))) + 3 * theme::kGap + theme::kPopupArrowSpace;
}

juce::Font RcvLookAndFeel::getPopupMenuFont()
{
    return theme::font();
}

// ----- tooltip --------------------------------------------------------------------------------

juce::Rectangle<int> RcvLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                                       juce::Rectangle<int> parentArea)
{
    const int w = static_cast<int> (std::ceil (theme::textWidth (theme::font(), tipText))) + 2 * theme::kGap + 2 * static_cast<int> (theme::kBorder);
    const int h = theme::kPopupItemHeight;
    const int x = screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + theme::kTooltipOffsetFlipped) : screenPos.x + theme::kTooltipOffsetX;
    const int y = screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + theme::kGap) : screenPos.y + theme::kTooltipOffsetY;
    return juce::Rectangle<int> (x, y, w, h).constrainedWithin (parentArea);
}

void RcvLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    g.fillAll (c::panel);
    g.setColour (c::border);
    g.drawRect (0, 0, width, height, static_cast<int> (theme::kBorder));
    g.setFont (theme::font());
    g.setColour (c::text);
    g.drawText (text, juce::Rectangle<int> (width, height).reduced (theme::kGap, 0), juce::Justification::centredLeft, false);
}

// ----- text editor, scroll bar, resizer -----------------------------------------------------

void RcvLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    g.setColour (editor.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), theme::kRadius);
}

void RcvLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (! editor.isEnabled())
        return;
    g.setColour (editor.hasKeyboardFocus (true) && ! editor.isReadOnly() ? c::accent : c::border);
    g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (0.5f * theme::kBorder), theme::kRadius, theme::kBorder);
}

juce::CaretComponent* RcvLookAndFeel::createCaretComponent (juce::Component* keyFocusOwner)
{
    auto* caret = new SteadyCaret (keyFocusOwner);
    caret->setColour (juce::CaretComponent::caretColourId, c::text);
    return caret;
}

void RcvLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool isVertical,
                                    int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown)
{
    const auto thumb = isVertical ? juce::Rectangle<int> (x, thumbStart, width, thumbSize)
                                  : juce::Rectangle<int> (thumbStart, y, thumbSize, height);
    g.setColour (isMouseOver || isMouseDown ? c::border : c::track);
    g.fillRoundedRectangle (thumb.reduced (theme::kScrollbarInset).toFloat(), theme::kRadius);
}

void RcvLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
{
    g.setColour (isMouseOver || isMouseDragging ? c::textDim : c::border);
    const float fw = static_cast<float> (w), fh = static_cast<float> (h);
    for (float i = 0.3f; i < 1.0f; i += 0.3f)
        g.drawLine (fw * i, fh, fw, fh * i, theme::kBorder);
}

} // namespace rcv
