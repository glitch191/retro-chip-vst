#include "ui/CommonStrip.h"

#include "ui/KeyboardFocus.h"
#include "ui/Theme.h"

#include "chipdsp/EngineFactory.h"

#include <algorithm>
#include <cmath>

namespace rcv
{

namespace
{
    constexpr int kImportId = 1000000;
    constexpr int kExportId = 1000001;
    constexpr int kImportSampleId = 1000002;
    const juce::String kScaleCaption ("UI scale");
    const juce::String kProductName ("Retro Chip");
    const juce::String kModelLine1 ("Chiptune");
    const juce::String kModelLine2 ("Sound module");

    // Text buttons show their label in silkscreen capitals; a toggle also has room for its LED.
    int textButtonWidth (const juce::Button& button)
    {
        const int text = static_cast<int> (std::ceil (theme::silkWidth (button.getButtonText())));
        if (button.getClickingTogglesState())
            return text + static_cast<int> (theme::kLedInsetX + theme::kLedWidth) + theme::kGap;
        return text + theme::kGap + theme::kUnit;
    }

    int captionWidth (const juce::String& text)
    {
        return static_cast<int> (std::ceil (theme::silkWidth (text)));
    }

    juce::Font productFont()
    {
        return theme::font (theme::kFontProduct, true).italicised();
    }

    int productWidth()
    {
        const auto f = theme::layoutFont (theme::kFontProduct, true).italicised();
        return static_cast<int> (std::ceil (theme::textWidth (f, kProductName))) + theme::kUnit;
    }

    // Short fixed-width chip label of a result row.
    const char* chipTag (chipdsp::ChipId chip)
    {
        switch (chip)
        {
            case chipdsp::ChipId::Nes: return "NES";
            case chipdsp::ChipId::Snes: return "SNES";
            case chipdsp::ChipId::Genesis: return "GEN";
        }
        return "";
    }

    // Header line of the results list: the number of results, or "No preset matches".
    class ResultsSummary final : public juce::Component
    {
    public:
        void setText (const juce::String& newText)
        {
            if (newText == text)
                return;
            text = newText;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (theme::colours::recess);
            g.setFont (theme::font());
            g.setColour (theme::colours::textDim);
            g.drawText (text, getLocalBounds().reduced (theme::kGap, 0), juce::Justification::centredLeft, true);
            g.setColour (theme::colours::separator);
            g.fillRect (0.0f, static_cast<float> (getHeight()) - theme::kBorder, static_cast<float> (getWidth()), theme::kBorder);
        }

    private:
        juce::String text;
    };
} // namespace

// ----- SearchField --------------------------------------------------------------------------

namespace
{
    bool isHostShortcut (const juce::ModifierKeys& mods)
    {
        return mods.isCtrlDown() || mods.isAltDown() || mods.isCommandDown();
    }
} // namespace

bool CommonStrip::SearchField::keyPressed (const juce::KeyPress& key)
{
    if (onNavigationKey && onNavigationKey (key))
        return true;
    const bool used = juce::TextEditor::keyPressed (key);
    return used || ! isHostShortcut (key.getModifiers());
}

bool CommonStrip::SearchField::keyStateChanged (bool isKeyDown)
{
    juce::TextEditor::keyStateChanged (isKeyDown);
    return ! isHostShortcut (juce::ModifierKeys::currentModifiers);
}

// ----- FormSection --------------------------------------------------------------------------

void FormSection::addRow (std::vector<juce::Component*> items, int height)
{
    items.erase (std::remove (items.begin(), items.end(), nullptr), items.end());   // parameter not in the layout
    if (items.empty())
        return;
    for (auto* c : items)
        addAndMakeVisible (c);
    rows.push_back ({ std::move (items), height });
}

void FormSection::setLabelWidth (int width)
{
    for (auto& row : rows)
        for (auto* c : row.items)
            if (auto* pc = dynamic_cast<ParamControl*> (c); pc != nullptr && pc->style() == ControlStyle::Inline)
                pc->setLabelWidth (width);
}

int FormSection::heightForWidth (int) const
{
    int h = theme::kGroupTitleHeight + theme::kPad - theme::kGap;
    for (const auto& row : rows)
        h += row.height + theme::kGap;
    return rows.empty() ? theme::kGroupTitleHeight + theme::kPad : h;
}

int FormSection::preferredWidth() const
{
    int w = 0;
    for (const auto& row : rows)
    {
        int rowW = -theme::kGap;
        for (auto* c : row.items)
            rowW += (dynamic_cast<ParamControl*> (c) != nullptr ? dynamic_cast<ParamControl*> (c)->preferredWidth() : 0) + theme::kGap;
        w = juce::jmax (w, rowW);
    }
    return w + 2 * theme::kPad;
}

void FormSection::resized()
{
    int y = theme::kGroupTitleHeight;
    const int contentW = getWidth() - 2 * theme::kPad;
    for (const auto& row : rows)
    {
        std::vector<juce::Component*> shown;
        for (auto* c : row.items)
            if (c->isVisible())
                shown.push_back (c);
        const int n = static_cast<int> (shown.size());
        const int w = (contentW - (n - 1) * theme::kGap) / juce::jmax (1, n);
        for (int i = 0; i < n; ++i)
            shown[static_cast<size_t> (i)]->setBounds (theme::kPad + i * (w + theme::kGap), y, i == n - 1 ? contentW - i * (w + theme::kGap) : w,
                                                       row.height);
        y += row.height + theme::kGap;
    }
}

// ----- CommonStrip --------------------------------------------------------------------------

CommonStrip::CommonStrip (RetroChipProcessor& p, UiContext& context)
    : processor (p), ctx (context)
{
    // Only the children take clicks: the chip panel under the strip's empty area stays usable.
    setInterceptsMouseClicks (false, true);

    // Only the search field takes keyboard focus; a click on a button focuses the strip, so
    // Space and the arrow keys keep reaching the host.
    setWantsKeyboardFocus (true);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &previousButton, &nextButton, &presetButton, &randomizeButton,
                                                                          &scaleBox, &diagnosticsButton })
        c->setWantsKeyboardFocus (false);

    const auto& registry = ctx.registry;

    // ----- header
    if (const auto* chipInfo = registry.find (ParamIds::chip))
    {
        chipChoice = std::make_unique<ParamChoice> (ctx, *chipInfo, "Chip", ControlStyle::Inline);
        addAndMakeVisible (*chipChoice);
    }

    previousButton.onClick = [this] { processor.presetManager().previous(); };
    nextButton.onClick = [this] { processor.presetManager().next(); };
    presetButton.getProperties().set ("rcvDropDown", true);
    presetButton.setTooltip ("Choose a preset by category and subcategory");
    presetButton.onClick = [this] { showPresetMenu(); };
    // Arrow buttons: the text stays the accessible name.
    previousButton.getProperties().set ("rcvArrow", -1);
    nextButton.getProperties().set ("rcvArrow", 1);
    previousButton.setTooltip ("Previous preset in the current list");
    nextButton.setTooltip ("Next preset in the current list");
    addAndMakeVisible (previousButton);
    addAndMakeVisible (presetButton);
    addAndMakeVisible (nextButton);

    searchField.setTextToShowWhenEmpty ("Search all presets", theme::colours::textDim);
    searchField.setFont (theme::font());
    searchField.setIndents (theme::kGap, 0);
    searchField.setJustification (juce::Justification::centredLeft);
    searchField.setSelectAllWhenFocused (true);
    searchField.setComponentID ("presetSearch");
    searchField.setTitle ("Preset search");
    searchField.setTooltip ("Search the presets of all three chips. Every word must match the name, category, subcategory, "
                            "a tag or the chip (nes, snes, genesis). Up and Down choose a result, Return loads it, Escape clears");
    searchField.onTextChange = [this] { updateSearch(); };
    searchField.onEscapeKey = [this]
    {
        searchField.clear();
        updateSearch();
        releaseSearchFocus();
    };
    searchField.onReturnKey = [this]
    {
        const int selected = resultsList.getSelectedRow();
        applyResult (isResultRow (selected) ? selected : firstResultRow());
    };
    searchField.onFocusLost = [this]
    {
        if (! resultsList.isMouseOver (true))
            resultsList.setVisible (false);
    };
    searchField.onNavigationKey = [this] (const juce::KeyPress& key) { return searchKeyPressed (key); };
    addAndMakeVisible (searchField);

    auto summary = std::make_unique<ResultsSummary>();
    resultsSummary = summary.get();
    resultsSummary->setSize (theme::kResultsWidth, theme::kPopupItemHeight);
    resultsList.setHeaderComponent (std::move (summary));
    resultsList.setRowHeight (theme::kPopupItemHeight);
    resultsList.setOutlineThickness (static_cast<int> (theme::kBorder));
    resultsList.setWantsKeyboardFocus (false);   // the search field keeps the keys
    addChildComponent (resultsList);

    randomAmount = std::make_unique<Knob> ("Amount", "Randomize amount", 0.0f, 100.0f,
                                           Randomizer::kDefaultAmount * 100.0f, 1.0f, "%", ControlStyle::Inline);
    addAndMakeVisible (*randomAmount);
    randomizeButton.setTooltip ("Draw new values for this chip's parameters around the current ones");
    randomizeButton.onClick = [this] { randomize(); };
    addAndMakeVisible (randomizeButton);

    for (int pct : theme::kScaleChoicesPercent)
        scaleBox.addItem (juce::String (pct) + " %", pct);
    scaleBox.setTooltip ("Size of the editor");
    scaleBox.setRepaintsOnMouseActivity (true);
    scaleBox.onChange = [this]
    {
        if (scaleBox.getSelectedId() > 0 && onScaleChosen)
            onScaleChosen (static_cast<float> (scaleBox.getSelectedId()) / 100.0f);
    };
    addAndMakeVisible (scaleBox);

    diagnosticsButton.setClickingTogglesState (true);
    diagnosticsButton.setTooltip ("Show refresh rate, frame times and repaint count");
    diagnosticsButton.onClick = [this]
    {
        if (onDiagnosticsToggled)
            onDiagnosticsToggled (diagnosticsButton.getToggleState());
    };
    addAndMakeVisible (diagnosticsButton);

    // ----- sidebar
    auto* arpEnable = addControl (arpSection, ParamIds::arpEnabled, "Enable", ControlStyle::Inline);
    auto* arpHold = addControl (arpSection, ParamIds::arpHold, "Hold", ControlStyle::Inline);
    arpSection.addRow ({ arpEnable, arpHold });
    arpSection.addRow ({ addControl (arpSection, ParamIds::arpPattern, "Pattern", ControlStyle::Inline) });
    arpSection.addRow ({ addControl (arpSection, ParamIds::arpOctaves, "Octaves", ControlStyle::Inline) });
    arpSection.addRow ({ addControl (arpSection, ParamIds::arpRateMode, "Rate", ControlStyle::Inline) });
    // Division (Sync) and Free rate (Free) share one row: only the one the rate mode uses is shown.
    arpDivision = addControl (arpSection, ParamIds::arpSyncDivision, "Division", ControlStyle::Inline);
    arpFreeRate = addControl (arpSection, ParamIds::arpFreeRate, "Free rate", ControlStyle::Inline);
    arpSection.addRow ({ arpDivision, arpFreeRate });
    arpSection.addRow ({ addControl (arpSection, ParamIds::arpGate, "Gate", ControlStyle::Inline) });

    glideSection.addRow ({ addControl (glideSection, ParamIds::glideTime, "Time", ControlStyle::Inline) });
    glideSection.addRow ({ addControl (glideSection, ParamIds::glideMode, "Mode", ControlStyle::Inline) });

    outputSection.addRow ({ addControl (outputSection, ParamIds::voiceMode, "Voice mode", ControlStyle::Inline) });
    polyChannels = std::make_unique<PolyChannelsControl> (ctx);
    outputSection.addRow ({ polyChannels.get() }, PolyChannelsControl::preferredHeight());
    outputSection.addRow ({ addControl (outputSection, ParamIds::rawOutput, "Raw output", ControlStyle::Inline) });
    outputSection.addRow ({ addControl (outputSection, ParamIds::masterGain, "Gain", ControlStyle::Inline) });

    int labelW = 0;
    for (const auto& c : sidebarControls)
        if (c->style() == ControlStyle::Inline && ! c->isHalfHeight())
            labelW = juce::jmax (labelW, c->labelWidth());
    for (auto* form : { &arpSection, &glideSection, &outputSection })
    {
        form->setLabelWidth (labelW);
        addAndMakeVisible (*form);
    }

    if (auto* rateMode = ctx.apvts.getParameter (ParamIds::arpRateMode))
    {
        rateModeAttachment = std::make_unique<juce::ParameterAttachment> (*rateMode, [this] (float) { updateArpRateControls(); });
        rateModeAttachment->sendInitialUpdate();
    }

    sampleStatus.setFont (theme::font());
    sampleStatus.setColour (juce::Label::textColourId, theme::colours::text);
    sampleStatus.setColour (juce::Label::backgroundColourId, theme::colours::recess);
    sampleStatus.setColour (juce::Label::outlineColourId, theme::colours::controlEdge);
    sampleStatus.setJustificationType (juce::Justification::topLeft);
    sampleStatus.setBorderSize (juce::BorderSize<int> (theme::kUnit, theme::kPad, theme::kUnit, theme::kPad));
    sampleStatus.setMinimumHorizontalScale (1.0f);
    addChildComponent (sampleStatus);

    processor.presetManager().addChangeListener (this);
    refreshPresetName();
    refreshSampleStatus();
}

void CommonStrip::refreshSampleStatus()
{
    const auto status = processor.presetManager().sampleStatus();
    const auto text = status.isEmpty() ? juce::String() : "Sample not loaded. " + status;
    if (text == sampleStatus.getText())
        return;
    sampleStatus.setText (text, juce::dontSendNotification);
    sampleStatus.setTooltip (text);   // the box may cut long names
    sampleStatus.setVisible (text.isNotEmpty());
}

CommonStrip::~CommonStrip()
{
    processor.presetManager().removeChangeListener (this);
}

bool CommonStrip::keepsKeyboardFocus (const juce::Component* c) const
{
    return c != nullptr && (c == &searchField || searchField.isParentOf (c) || c == &resultsList || resultsList.isParentOf (c));
}

bool CommonStrip::searchKeyPressed (const juce::KeyPress& key)
{
    // Up / Down move the selection of the results list while the search field has focus.
    if (key == juce::KeyPress::downKey)
        return moveSelection (1);
    if (key == juce::KeyPress::upKey)
        return moveSelection (-1);
    return false;
}

bool CommonStrip::moveSelection (int direction)
{
    if (! resultsList.isVisible() || numResults == 0)
        return false;
    const int count = static_cast<int> (resultRows.size());
    int row = resultsList.getSelectedRow();
    if (! isResultRow (row))
        row = direction > 0 ? -1 : count;
    for (int r = row + direction; r >= 0 && r < count; r += direction)
    {
        if (! isResultRow (r))
            continue;
        resultsList.selectRow (r);
        if (r > 0 && ! isResultRow (r - 1))
            resultsList.scrollToEnsureRowIsOnscreen (r - 1);   // keep the chip header in view
        break;
    }
    return true;   // at either end the selection stays
}

void CommonStrip::releaseSearchFocus()
{
    // No component keeps the focus and the host gets the keyboard back (caret hidden).
    releaseKeyboardFocus (*this);
}

void CommonStrip::setMaxScale (float maxScale)
{
    for (int pct : theme::kScaleChoicesPercent)
        scaleBox.setItemEnabled (pct, static_cast<float> (pct) <= maxScale * 100.0f + 0.5f);
}

ParamControl* CommonStrip::addControl (FormSection&, const char* paramId, const juce::String& label, ControlStyle style)
{
    const auto* info = ctx.registry.find (paramId);
    if (info == nullptr)
        return nullptr;
    sidebarControls.push_back (createParamControl (ctx, *info, label, style));
    return sidebarControls.back().get();
}

void CommonStrip::updateArpRateControls()
{
    const auto* raw = ctx.apvts.getRawParameterValue (ParamIds::arpRateMode);
    const bool free = raw != nullptr && raw->load() > 0.5f;
    if (arpDivision != nullptr)
        arpDivision->setVisible (! free);
    if (arpFreeRate != nullptr)
        arpFreeRate->setVisible (free);
    arpSection.resized();
}

void CommonStrip::setAreas (juce::Rectangle<int> headerArea, juce::Rectangle<int> sidebarArea)
{
    header = headerArea;
    sidebar = sidebarArea;
    resized();
}

void CommonStrip::resized()
{
    if (header.isEmpty())
        return;

    // ----- header, left to right; the preset name takes the remaining width
    auto row = header.reduced (theme::kPad, 0).withSizeKeepingCentre (header.getWidth() - 2 * theme::kPad, theme::kControlHeight);

    // right side first
    diagnosticsButton.setBounds (row.removeFromRight (textButtonWidth (diagnosticsButton)));
    row.removeFromRight (theme::kHeaderGap);
    scaleBox.setBounds (row.removeFromRight (theme::kScaleBoxWidth));
    row.removeFromRight (theme::kGap + captionWidth (kScaleCaption));   // caption painted by paint()
    row.removeFromRight (theme::kHeaderGap);
    randomizeButton.setBounds (row.removeFromRight (textButtonWidth (randomizeButton)));
    row.removeFromRight (theme::kGap);
    randomAmount->setBounds (row.removeFromRight (randomAmount->preferredWidth()));
    row.removeFromRight (theme::kHeaderGap);

    // Product name and model line, painted by paint().
    row.removeFromLeft (productWidth() + theme::kGap + theme::kModelLineWidth + theme::kHeaderGap);
    if (chipChoice != nullptr)
        chipChoice->setBounds (row.removeFromLeft (chipChoice->preferredWidth()));
    row.removeFromLeft (theme::kHeaderGap);
    previousButton.setBounds (row.removeFromLeft (theme::kControlHeight));
    row.removeFromLeft (theme::kUnit);
    searchField.setBounds (row.removeFromRight (theme::kSearchWidth));
    row.removeFromRight (theme::kGap);
    nextButton.setBounds (row.removeFromRight (theme::kControlHeight));
    row.removeFromRight (theme::kUnit);
    presetButton.setBounds (row);

    // Results list: right-aligned to the field, header line + up to kResultsMaxRows rows.
    const int visibleRows = juce::jlimit (0, theme::kResultsMaxRows, getNumRows());
    const int listX = juce::jmax (header.getX(), searchField.getRight() - theme::kResultsWidth);
    resultsList.setBounds (listX, header.getBottom() + theme::kUnit, searchField.getRight() - listX,
                           theme::kPopupItemHeight + visibleRows * theme::kPopupItemHeight + 2 * static_cast<int> (theme::kBorder));

    // ----- sidebar
    auto side = sidebar;
    for (auto* form : { &arpSection, &glideSection, &outputSection })
    {
        form->setBounds (side.removeFromTop (form->heightForWidth (side.getWidth())));
        side.removeFromTop (theme::kGap);
    }
    // Up to five lines of text in the space left under the forms.
    sampleStatus.setBounds (side.removeFromTop (juce::jmin (side.getHeight(), 5 * theme::kLabelHeight + 2 * theme::kUnit)));
}

void CommonStrip::paint (juce::Graphics& g)
{
    // On the faceplate: a dark rule under the header row.
    g.setColour (theme::colours::rule);
    g.fillRect (header.getX(), header.getBottom() + theme::kUnit - 1, header.getWidth(), 1);

    // Product name in large italic light blue, model line in two rows of small capitals beside it.
    auto brand = header.reduced (theme::kPad, 0);
    g.setColour (theme::colours::silk);
    g.setFont (productFont());
    g.drawFittedText (kProductName, brand.removeFromLeft (productWidth()), juce::Justification::centredLeft, 1, theme::kMinHorizontalScale);
    brand.removeFromLeft (theme::kGap);
    auto model = brand.removeFromLeft (theme::kModelLineWidth).withSizeKeepingCentre (theme::kModelLineWidth, 2 * theme::kLabelHeight);
    g.setFont (theme::nameFont());
    g.drawFittedText (kModelLine1.toUpperCase(), model.removeFromTop (theme::kLabelHeight), juce::Justification::bottomLeft, 1, theme::kMinHorizontalScale);
    g.drawFittedText (kModelLine2.toUpperCase(), model, juce::Justification::topLeft, 1, theme::kMinHorizontalScale);

    g.setFont (theme::nameFont());
    g.setColour (theme::colours::silkOrange);
    const int captionW = captionWidth (kScaleCaption);
    g.drawFittedText (kScaleCaption.toUpperCase(), scaleBox.getX() - theme::kGap - captionW, scaleBox.getY(), captionW, scaleBox.getHeight(),
                      juce::Justification::centredLeft, 1, theme::kMinHorizontalScale);
}

// ----- chip / scale / diagnostics -----------------------------------------------------------

void CommonStrip::chipChanged()
{
    browseCategory.clear();
    browseSubcategory.clear();
    // A preset of another chip (search result, Previous/Next through cross-chip results,
    // import) keeps the search and the list being walked; the chip selector clears them.
    if (! processor.presetManager().isApplying())
    {
        searchField.clear();
        resultRows.clear();
        numResults = 0;
        resultsList.setVisible (false);
        restoreBrowseList();
    }
    refreshPresetName();
}

void CommonStrip::setDisplayedScale (float scale)
{
    const int pct = juce::roundToInt (scale * 100.0f);
    if (scaleBox.indexOfItemId (pct) >= 0)
        scaleBox.setSelectedId (pct, juce::dontSendNotification);
    else
        scaleBox.setText (juce::String (pct) + " %", juce::dontSendNotification);
}

void CommonStrip::setDiagnosticsShown (bool shown)
{
    diagnosticsButton.setToggleState (shown, juce::dontSendNotification);
}

void CommonStrip::setSearchText (const juce::String& text)
{
    searchField.setText (text, juce::dontSendNotification);
    updateSearch();
}

void CommonStrip::randomize()
{
    const auto seed = static_cast<juce::uint32> (juce::Random::getSystemRandom().nextInt());
    processor.randomizer().randomize (processor.selectedChip(), randomAmount->getValue() / 100.0f, seed);
}

// ----- preset browser -----------------------------------------------------------------------

void CommonStrip::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshPresetName();
    refreshSampleStatus();
}

void CommonStrip::refreshPresetName()
{
    auto& pm = processor.presetManager();
    const auto chip = processor.selectedChip();
    const bool any = pm.numPresets (chip) > 0;
    const auto* current = pm.current();

    juce::String text;
    const bool named = current != nullptr && current->chip == chip;
    if (named)
        text = current->name;
    else
        text = any ? "Choose a preset" : "No presets for this chip";
    presetButton.setButtonText (text);
    // The name may be cut with an ellipsis: the tooltip always carries it in full.
    presetButton.setTooltip (named ? "Preset: " + current->name + ". Click to choose another by category and subcategory"
                                   : juce::String ("Choose a preset by category and subcategory"));
    presetButton.setEnabled (any);
    previousButton.setEnabled (any);
    nextButton.setEnabled (any);
    searchField.setEnabled (pm.numPresets() > 0);   // the search covers every chip
}

void CommonStrip::restoreBrowseList()
{
    auto& pm = processor.presetManager();
    const auto chip = processor.selectedChip();
    if (searchField.getText().trim().isNotEmpty())
        pm.searchAll (searchField.getText());
    else
        pm.presets (chip, browseCategory, browseSubcategory);
}

void CommonStrip::showPresetMenu()
{
    auto& pm = processor.presetManager();
    const auto chip = processor.selectedChip();
    const auto* current = pm.current();
    const juce::String currentName = current != nullptr && current->chip == chip ? current->name : juce::String();
    const juce::String currentCategory = current != nullptr && current->chip == chip ? current->category : juce::String();
    const juce::String currentSub = current != nullptr && current->chip == chip ? current->subcategory : juce::String();

    auto entries = std::make_shared<std::vector<MenuEntry>>();
    auto addPresets = [&] (juce::PopupMenu& menu, const juce::String& category, const juce::String& sub, bool onlyWithoutSub)
    {
        for (const auto* preset : pm.presets (chip, category, sub))
        {
            if (onlyWithoutSub && preset->subcategory.isNotEmpty())
                continue;
            if (category.isEmpty() && preset->category.isNotEmpty())
                continue;
            entries->push_back ({ category, sub, preset->name });
            menu.addItem (static_cast<int> (entries->size()), preset->name, true, preset->name == currentName);
        }
    };

    juce::PopupMenu menu;
    for (const auto& category : pm.categories (chip))
    {
        juce::PopupMenu categoryMenu;
        for (const auto& sub : pm.subcategories (chip, category))
        {
            juce::PopupMenu subMenu;
            addPresets (subMenu, category, sub, false);
            categoryMenu.addSubMenu (sub, subMenu, true, nullptr, category == currentCategory && sub == currentSub);
        }
        addPresets (categoryMenu, category, {}, true);
        menu.addSubMenu (category, categoryMenu, true, nullptr, category == currentCategory);
    }
    addPresets (menu, {}, {}, true);   // presets without a category
    if (entries->empty())
        menu.addItem (-1, "No presets for this chip", false);

    menu.addSeparator();
    menu.addItem (kImportId, "Import preset...");
    menu.addItem (kExportId, "Export current settings...");

    // User WAV import into the slot the chip's sample parameter selects (DMC, BRR or DAC).
    const int slot = pm.currentSampleSlot (chip);
    menu.addItem (kImportSampleId, slot >= 0 ? "Import sample into slot " + juce::String (slot) + "..."
                                             : juce::String ("Import sample (this engine has no sample slots)"),
                  slot >= 0);

    restoreBrowseList();   // building the menu queried other lists

    // A PopupMenu uses the default LookAndFeel unless it is given one (its submenus inherit it).
    menu.setLookAndFeel (&getLookAndFeel());

    juce::Component::SafePointer<CommonStrip> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetButton)
                                                  .withMinimumWidth (presetButton.getWidth())
                                                  .withStandardItemHeight (theme::kPopupItemHeight),
                        [safeThis, entries] (int result)
                        {
                            if (safeThis == nullptr)
                                return;
                            if (result == kImportId)
                                safeThis->importPreset();
                            else if (result == kExportId)
                                safeThis->exportPreset();
                            else if (result == kImportSampleId)
                                safeThis->importSample();
                            else if (result >= 1 && result <= static_cast<int> (entries->size()))
                                safeThis->choosePreset ((*entries)[static_cast<size_t> (result - 1)]);
                        });
}

void CommonStrip::choosePreset (const MenuEntry& entry)
{
    auto& pm = processor.presetManager();
    const auto chip = processor.selectedChip();
    browseCategory = entry.category;
    browseSubcategory = entry.subcategory;
    searchField.clear();
    resultsList.setVisible (false);

    for (const auto* preset : pm.presets (chip, browseCategory, browseSubcategory))
    {
        if (preset->name == entry.name)
        {
            pm.apply (*preset);
            return;
        }
    }
}

void CommonStrip::updateSearch()
{
    auto& pm = processor.presetManager();
    const auto text = searchField.getText().trim();
    resultRows.clear();
    numResults = 0;
    if (text.isEmpty())
    {
        resultsList.setVisible (false);
        restoreBrowseList();
        return;
    }

    // All chips, sorted by chip then category, subcategory, name; one header row per chip.
    const auto found = pm.searchAll (text);
    numResults = static_cast<int> (found.size());
    size_t headerIndex = 0;
    for (const auto* preset : found)
    {
        if (resultRows.empty() || resultRows.back().chip != preset->chip)
        {
            headerIndex = resultRows.size();
            resultRows.push_back ({ preset->chip, {}, {}, 0 });
        }
        ++resultRows[headerIndex].count;
        juce::String location = preset->category;
        if (preset->subcategory.isNotEmpty())
            location << (location.isNotEmpty() ? " / " : "") << preset->subcategory;
        resultRows.push_back ({ preset->chip, preset->name, location, 0 });
    }

    // "193 presets match: NES 61, SNES 41, Genesis 91" (every chip is named even when only
    // the first group fits in the list).
    juce::String summary ("No preset matches");
    if (numResults > 0)
    {
        juce::StringArray perChip;
        for (const auto& r : resultRows)
            if (r.isHeader())
                perChip.add (juce::String (chipdsp::chipName (r.chip)) + " " + juce::String (r.count));
        summary = juce::String (numResults) + (numResults == 1 ? " preset matches: " : " presets match: ") + perChip.joinIntoString (", ");
    }
    static_cast<ResultsSummary*> (resultsSummary)->setText (summary);
    resultsList.deselectAllRows();
    resultsList.updateContent();
    resultsList.getVerticalScrollBar().setCurrentRangeStart (0.0);
    resized();
    resultsList.setVisible (true);
    resultsList.repaint();
}

bool CommonStrip::isResultRow (int row) const
{
    return juce::isPositiveAndBelow (row, static_cast<int> (resultRows.size())) && ! resultRows[static_cast<size_t> (row)].isHeader();
}

int CommonStrip::firstResultRow() const
{
    for (int r = 0; r < static_cast<int> (resultRows.size()); ++r)
        if (isResultRow (r))
            return r;
    return -1;
}

int CommonStrip::getNumRows()
{
    return static_cast<int> (resultRows.size());
}

CommonStrip::RowLayout CommonStrip::rowLayout (int width, int height)
{
    // | 8 | chip tag 44 | 8 | name ... | 8 | Category / Subcategory 160 | 8 |
    RowLayout l;
    auto area = juce::Rectangle<int> (0, 0, width, height).reduced (theme::kGap, 0);
    l.tag = area.removeFromLeft (theme::kChipTagWidth).withSizeKeepingCentre (theme::kChipTagWidth, theme::kChipTagHeight);
    area.removeFromLeft (theme::kGap);
    l.location = area.removeFromRight (juce::jmin (theme::kResultsCategoryWidth, area.getWidth() / 2));
    area.removeFromRight (theme::kGap);
    l.name = area;
    return l;
}

void CommonStrip::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, static_cast<int> (resultRows.size())))
        return;
    const auto& r = resultRows[static_cast<size_t> (row)];

    if (r.isHeader())
    {
        // Chip header: "SNES (41)" in silkscreen capitals on the recessed list.
        g.setFont (theme::silkFont());
        g.setColour (theme::colours::silk);
        g.drawText (juce::String (chipdsp::chipName (r.chip)).toUpperCase() + " (" + juce::String (r.count) + ")", theme::kGap, 0,
                    width - 2 * theme::kGap, height, juce::Justification::centredLeft, true);
        return;
    }

    // Selection: blue with amber text.
    if (selected)
        g.fillAll (theme::colours::listBlue);
    const auto l = rowLayout (width, height);

    // Chip tag: a small dark plate with silkscreen capitals.
    theme::drawButtonFace (g, l.tag.toFloat(), true, 0.0f, false);
    g.setFont (theme::silkFont());
    g.setColour (theme::colours::text);
    g.drawText (chipTag (r.chip), l.tag, juce::Justification::centred, false);

    g.setFont (theme::font());
    g.setColour (selected ? theme::colours::listAmber : theme::colours::text);
    g.drawText (r.name, l.name, juce::Justification::centredLeft, true);
    g.setColour (selected ? theme::colours::listAmber : theme::colours::textDim);
    g.drawText (r.location, l.location, juce::Justification::centredLeft, true);
}

void CommonStrip::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    applyResult (row);   // header rows do nothing
}

void CommonStrip::selectedRowsChanged (int lastRowSelected)
{
    // Header rows cannot be selected.
    if (lastRowSelected >= 0 && ! isResultRow (lastRowSelected))
        resultsList.deselectRow (lastRowSelected);
}

void CommonStrip::applyResult (int row)
{
    if (! isResultRow (row))
        return;
    const auto& r = resultRows[static_cast<size_t> (row)];
    auto& pm = processor.presetManager();
    // A result of another chip switches the chip (apply() writes `chip` last) and the panel
    // follows; chipChanged() keeps the search because the change comes from apply().
    if (const auto* preset = pm.findByName (r.chip, r.name))
        pm.apply (*preset);
    // The results stay the Previous/Next list; the list closes and the field lets go of the
    // keyboard. Typing in the field again reopens the list.
    resultsList.selectRow (row);
    resultsList.setVisible (false);
    releaseSearchFocus();
}

juce::String CommonStrip::getTooltipForRow (int row)
{
    // Only when the name or the location is cut with an ellipsis: the full text.
    if (! isResultRow (row))
        return {};
    const auto& r = resultRows[static_cast<size_t> (row)];
    const auto l = rowLayout (resultsList.getVisibleRowWidth(), theme::kPopupItemHeight);
    const auto f = theme::font();
    const bool cut = theme::textWidth (f, r.name) > static_cast<float> (l.name.getWidth())
                  || theme::textWidth (f, r.location) > static_cast<float> (l.location.getWidth());
    if (! cut)
        return {};
    return r.name + " (" + juce::String (chipdsp::chipName (r.chip)) + ", " + r.location + ")";
}

void CommonStrip::importSample()
{
    const auto chip = processor.selectedChip();
    const int slot = processor.presetManager().currentSampleSlot (chip);
    if (slot < 0)
        return;
    fileChooser = std::make_unique<juce::FileChooser> ("Import sample into slot " + juce::String (slot),
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.wav");
    juce::Component::SafePointer<CommonStrip> safeThis (this);
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safeThis, chip, slot] (const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();
                                  if (safeThis == nullptr || ! file.existsAsFile())
                                      return;
                                  auto& pm = safeThis->processor.presetManager();
                                  if (! pm.importUserSample (chip, slot, file))
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Import sample",
                                                                              "The file could not be loaded into slot " + juce::String (slot)
                                                                                  + ". " + pm.sampleStatus() + ".");
                              });
}

void CommonStrip::importPreset()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Import preset", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.json");
    juce::Component::SafePointer<CommonStrip> safeThis (this);
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safeThis] (const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();
                                  if (safeThis != nullptr && file.existsAsFile())
                                      safeThis->processor.presetManager().importFile (file);
                              });
}

void CommonStrip::exportPreset()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Export current settings", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.json");
    juce::Component::SafePointer<CommonStrip> safeThis (this);
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safeThis] (const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();
                                  if (safeThis != nullptr && file != juce::File())
                                      safeThis->processor.presetManager().exportCurrent (file.withFileExtension ("json"));
                              });
}

} // namespace rcv
