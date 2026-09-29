#include "ui/CommonStrip.h"

#include "ui/Theme.h"

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

    int textButtonWidth (const juce::String& text)
    {
        return static_cast<int> (std::ceil (theme::textWidth (theme::font(), text))) + 2 * theme::kGap + theme::kUnit;
    }

    int captionWidth (const juce::String& text)
    {
        return static_cast<int> (std::ceil (theme::textWidth (theme::font(), text)));
    }
} // namespace

// ----- FormSection --------------------------------------------------------------------------

void FormSection::addRow (std::vector<juce::Component*> items)
{
    items.erase (std::remove (items.begin(), items.end(), nullptr), items.end());   // parameter not in the layout
    if (items.empty())
        return;
    for (auto* c : items)
        addAndMakeVisible (c);
    rows.push_back (std::move (items));
}

void FormSection::setLabelWidth (int width)
{
    for (auto& row : rows)
        for (auto* c : row)
            if (auto* pc = dynamic_cast<ParamControl*> (c); pc != nullptr && pc->style() == ControlStyle::Inline)
                pc->setLabelWidth (width);
}

int FormSection::heightForWidth (int) const
{
    const int n = static_cast<int> (rows.size());
    return theme::kGroupTitleHeight + n * theme::kControlHeight + juce::jmax (0, n - 1) * theme::kGap + theme::kPad;
}

int FormSection::preferredWidth() const
{
    int w = 0;
    for (const auto& row : rows)
    {
        int rowW = -theme::kGap;
        for (auto* c : row)
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
        const int n = static_cast<int> (row.size());
        const int w = (contentW - (n - 1) * theme::kGap) / juce::jmax (1, n);
        for (int i = 0; i < n; ++i)
            row[static_cast<size_t> (i)]->setBounds (theme::kPad + i * (w + theme::kGap), y, i == n - 1 ? contentW - i * (w + theme::kGap) : w,
                                                     theme::kControlHeight);
        y += theme::kControlHeight + theme::kGap;
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
    previousButton.setTooltip ("Previous preset in the current list");
    nextButton.setTooltip ("Next preset in the current list");
    addAndMakeVisible (previousButton);
    addAndMakeVisible (presetButton);
    addAndMakeVisible (nextButton);

    searchField.setTextToShowWhenEmpty ("Search presets", theme::colours::textDim);
    searchField.setFont (theme::font());
    searchField.setIndents (theme::kGap, 0);
    searchField.setJustification (juce::Justification::centredLeft);
    searchField.setSelectAllWhenFocused (true);
    searchField.setTooltip ("Filter presets by name and tags. Up and Down choose a match, Return loads it");
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
        applyResult (selected >= 0 ? selected : 0);
    };
    searchField.onFocusLost = [this]
    {
        if (! resultsList.isMouseOver (true))
            resultsList.setVisible (false);
    };
    searchField.addKeyListener (&searchKeys);
    addAndMakeVisible (searchField);

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
    arpDivision = addControl (arpSection, ParamIds::arpSyncDivision, "Division", ControlStyle::Inline);
    arpSection.addRow ({ arpDivision });
    arpFreeRate = addControl (arpSection, ParamIds::arpFreeRate, "Free rate", ControlStyle::Inline);
    arpSection.addRow ({ arpFreeRate });
    arpSection.addRow ({ addControl (arpSection, ParamIds::arpGate, "Gate", ControlStyle::Inline) });

    glideSection.addRow ({ addControl (glideSection, ParamIds::glideTime, "Time", ControlStyle::Inline) });
    glideSection.addRow ({ addControl (glideSection, ParamIds::glideMode, "Mode", ControlStyle::Inline) });

    outputSection.addRow ({ addControl (outputSection, ParamIds::voiceMode, "Voice mode", ControlStyle::Inline) });
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

    processor.presetManager().addChangeListener (this);
    refreshPresetName();
}

CommonStrip::~CommonStrip()
{
    searchField.removeKeyListener (&searchKeys);
    processor.presetManager().removeChangeListener (this);
}

bool CommonStrip::searchKeyPressed (const juce::KeyPress& key)
{
    // Up / Down move the selection of the results list while the search field has focus.
    if (! resultsList.isVisible() || results.empty())
        return false;
    const int count = static_cast<int> (results.size());
    const int selected = resultsList.getSelectedRow();
    if (key == juce::KeyPress::downKey)
    {
        const int row = selected < 0 ? 0 : juce::jmin (count - 1, selected + 1);
        resultsList.selectRow (row);
        return true;
    }
    if (key == juce::KeyPress::upKey)
    {
        const int row = selected < 0 ? count - 1 : juce::jmax (0, selected - 1);
        resultsList.selectRow (row);
        return true;
    }
    return false;
}

void CommonStrip::releaseSearchFocus()
{
    // Focus moves to the editor content (it wants focus but handles no keys), so the caret
    // disappears and keys go back to the host.
    if (searchField.hasKeyboardFocus (true))
    {
        if (auto* parent = getParentComponent(); parent != nullptr && parent->getWantsKeyboardFocus())
            parent->grabKeyboardFocus();
        else
            searchField.giveAwayKeyboardFocus();
    }
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
        arpDivision->setEnabled (! free);
    if (arpFreeRate != nullptr)
        arpFreeRate->setEnabled (free);
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
    diagnosticsButton.setBounds (row.removeFromRight (textButtonWidth (diagnosticsButton.getButtonText())));
    row.removeFromRight (theme::kClusterGap);
    scaleBox.setBounds (row.removeFromRight (theme::kScaleBoxWidth));
    row.removeFromRight (theme::kGap + captionWidth (kScaleCaption));   // caption painted by paint()
    row.removeFromRight (theme::kClusterGap);
    randomizeButton.setBounds (row.removeFromRight (textButtonWidth (randomizeButton.getButtonText())));
    row.removeFromRight (theme::kGap);
    randomAmount->setBounds (row.removeFromRight (randomAmount->preferredWidth()));
    row.removeFromRight (theme::kClusterGap);

    if (chipChoice != nullptr)
        chipChoice->setBounds (row.removeFromLeft (chipChoice->preferredWidth()));
    row.removeFromLeft (theme::kClusterGap);
    previousButton.setBounds (row.removeFromLeft (textButtonWidth (previousButton.getButtonText())));
    row.removeFromLeft (theme::kUnit);
    searchField.setBounds (row.removeFromRight (theme::kSearchWidth));
    row.removeFromRight (theme::kGap);
    nextButton.setBounds (row.removeFromRight (textButtonWidth (nextButton.getButtonText())));
    row.removeFromRight (theme::kUnit);
    presetButton.setBounds (row);

    const int visibleRows = juce::jlimit (1, theme::kResultsMaxRows, getNumRows());
    resultsList.setBounds (searchField.getRight() - theme::kResultsWidth, header.getBottom() + theme::kUnit,
                           theme::kResultsWidth, visibleRows * theme::kPopupItemHeight + 2 * static_cast<int> (theme::kBorder));

    // ----- sidebar
    auto side = sidebar;
    for (auto* form : { &arpSection, &glideSection, &outputSection })
    {
        form->setBounds (side.removeFromTop (form->heightForWidth (side.getWidth())));
        side.removeFromTop (theme::kGap);
    }
}

void CommonStrip::paint (juce::Graphics& g)
{
    g.setColour (theme::colours::panel);
    g.fillRoundedRectangle (header.toFloat(), theme::kRadius);
    g.setColour (theme::colours::divider);
    g.drawRoundedRectangle (header.toFloat().reduced (0.5f * theme::kBorder), theme::kRadius, theme::kBorder);

    g.setFont (theme::font());
    g.setColour (theme::colours::text);
    const int captionW = captionWidth (kScaleCaption);
    g.drawText (kScaleCaption, scaleBox.getX() - theme::kGap - captionW, scaleBox.getY(), captionW, scaleBox.getHeight(),
                juce::Justification::centredLeft, false);
}

// ----- chip / scale / diagnostics -----------------------------------------------------------

void CommonStrip::chipChanged()
{
    browseCategory.clear();
    browseSubcategory.clear();
    searchField.clear();
    results.clear();
    resultsList.setVisible (false);
    restoreBrowseList();
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
    searchField.setEnabled (any);
}

void CommonStrip::restoreBrowseList()
{
    auto& pm = processor.presetManager();
    const auto chip = processor.selectedChip();
    if (searchField.getText().trim().isNotEmpty())
        pm.search (chip, searchField.getText());
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
    const auto chip = processor.selectedChip();
    const auto text = searchField.getText().trim();
    results.clear();
    if (text.isEmpty())
    {
        resultsList.setVisible (false);
        restoreBrowseList();
        return;
    }
    for (const auto* preset : pm.search (chip, text))
        results.push_back (preset->name);
    resultsList.updateContent();
    resized();
    resultsList.setVisible (true);
    resultsList.repaint();
}

int CommonStrip::getNumRows()
{
    return results.empty() ? 1 : static_cast<int> (results.size());
}

void CommonStrip::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    g.setFont (theme::font());
    if (results.empty())
    {
        g.setColour (theme::colours::textDim);
        g.drawText ("No matching presets", theme::kGap, 0, width - 2 * theme::kGap, height, juce::Justification::centredLeft, true);
        return;
    }
    if (selected)
        g.fillAll (theme::colours::surfaceHover);
    g.setColour (theme::colours::text);
    g.drawText (results[static_cast<size_t> (row)], theme::kGap, 0, width - 2 * theme::kGap, height, juce::Justification::centredLeft, true);
}

void CommonStrip::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    applyResult (row);
}

void CommonStrip::applyResult (int row)
{
    if (! juce::isPositiveAndBelow (row, static_cast<int> (results.size())))
        return;
    auto& pm = processor.presetManager();
    if (const auto* preset = pm.findByName (processor.selectedChip(), results[static_cast<size_t> (row)]))
        pm.apply (*preset);
    // The matches stay the Previous/Next list; the list closes and the field lets go of the
    // keyboard. Typing in the field again reopens the list.
    resultsList.selectRow (row);
    resultsList.setVisible (false);
    releaseSearchFocus();
}

juce::String CommonStrip::getTooltipForRow (int row)
{
    // Rows can be cut with an ellipsis in the 320 px list: the tooltip is the full name.
    if (juce::isPositiveAndBelow (row, static_cast<int> (results.size())))
        return results[static_cast<size_t> (row)];
    return {};
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
                                  if (! safeThis->processor.presetManager().importUserSample (chip, slot, file))
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Import sample",
                                                                              "The file could not be loaded into slot " + juce::String (slot)
                                                                                  + ": it is not a readable WAV file, or it does not fit the chip's sample memory.");
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
