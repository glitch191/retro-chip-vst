#pragma once

#include "PluginProcessor.h"
#include "ui/Knob.h"
#include "ui/ParamChoice.h"
#include "ui/ParamGroup.h"
#include "ui/ParamToggle.h"
#include "ui/PolyChannels.h"
#include "ui/Theme.h"

#include <functional>
#include <memory>
#include <vector>

namespace rcv
{

// A titled box of label + control rows (the sidebar forms). Each row holds one control or
// several side by side with equal widths (hidden ones take no width); inline controls share
// one label column. Rows are kControlHeight high unless addRow() is given a height.
class FormSection final : public PanelSection
{
public:
    explicit FormSection (juce::String titleIn) : PanelSection (std::move (titleIn)) {}

    void addRow (std::vector<juce::Component*> items, int height = theme::kControlHeight);
    void setLabelWidth (int width);

    int heightForWidth (int width) const override;
    int preferredWidth() const override;

    void paint (juce::Graphics& g) override { paintBox (g); }
    void resized() override;

private:
    struct Row
    {
        std::vector<juce::Component*> items;
        int height = 0;
    };
    std::vector<Row> rows;
};

// Controls shared by the three chips (docs/PLUGIN_SPECS.md "CommonStrip"):
//
//   header:  chip selector, preset browser (previous / name menu / next, search field with a
//            results list), randomize amount and button, UI scale, Diagnostics toggle;
//   sidebar: arpeggiator (enable, hold, pattern, octaves, rate mode, division or free rate
//            in one row, gate), glide (time, mode) and output (voice mode, poly channels,
//            raw output, master gain).
//
// The strip covers the whole editor content but only its children take mouse clicks, so the
// chip panel between the header and the sidebar stays reachable. The preset menu is two
// levels deep: category -> subcategory -> preset (presets without a subcategory sit in the
// category menu); choosing a preset makes its subcategory the list Previous/Next walk
// through.
//
// Search: typing in the search field searches the presets of all three chips
// (PresetManager::searchAll(), rule in Preset::matches()) on the message thread, at each
// text change, and lists them under the field grouped by chip ("SNES (41)" header rows),
// each row showing a chip tag, the name and "Category / Subcategory"; the list's header
// line gives the total or "No preset matches". The matches become the Previous/Next list.
// Choosing a result (click, or Up/Down then Return) applies it; a result of another chip
// switches the chip (PresetManager::apply() writes `chip`) and the panel follows, the
// search text and list stay. A chip change made with the chip selector clears the search.
//
// Keyboard: the search field is the only component that takes keys. While focused it
// consumes every key (SearchField), so the host does not also receive them; Escape,
// Return on a result and a click anywhere else give the keyboard back to the host
// (releaseKeyboardFocus()).
class CommonStrip final : public juce::Component,
                          private juce::ChangeListener,
                          private juce::ListBoxModel
{
public:
    // The preset search field. While it has the keyboard focus it consumes every key:
    // keyPressed() and keyStateChanged() return true, so JUCE reports each key down, up and
    // character as handled and the host does not also receive it (on Windows the wrapper's
    // message hook then swallows the key). Only combinations with Ctrl or Alt that the text
    // editor does not use (host shortcuts such as Ctrl+S, Alt+F4) pass through.
    class SearchField final : public juce::TextEditor
    {
    public:
        std::function<bool (const juce::KeyPress&)> onNavigationKey;   // first refusal on every key press
        bool keyPressed (const juce::KeyPress& key) override;
        bool keyStateChanged (bool isKeyDown) override;
    };

    CommonStrip (RetroChipProcessor& processor, UiContext& ctx);
    ~CommonStrip() override;

    void setAreas (juce::Rectangle<int> headerArea, juce::Rectangle<int> sidebarArea);

    // The editor tells the strip about chip and scale changes and receives user choices.
    void chipChanged();
    void setDisplayedScale (float scale);
    void setMaxScale (float maxScale);   // UI scale entries larger than the screen are disabled
    std::function<void (float)> onScaleChosen;
    std::function<void (bool)> onDiagnosticsToggled;
    void setDiagnosticsShown (bool shown);
    void setSearchText (const juce::String& text);   // as if typed (screenshot hook)
    void openPresetMenu() { showPresetMenu(); }      // as if clicked (screenshot hook)

    // True for the search field and the results list (and their children): a click there
    // keeps the keyboard in the field; a click anywhere else in the editor releases it.
    bool keepsKeyboardFocus (const juce::Component* c) const;
    SearchField& presetSearchField() noexcept { return searchField; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    // Preset browser
    struct MenuEntry
    {
        juce::String category, subcategory, name;
    };
    void showPresetMenu();
    void choosePreset (const MenuEntry& entry);
    void refreshPresetName();
    void updateSearch();
    void restoreBrowseList();
    void importPreset();
    void exportPreset();
    void importSample();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // Results list (juce::ListBoxModel): chip header rows and preset rows. Keyboard: Up/Down
    // in the search field move the selection over the preset rows, Return loads the
    // selected result (the first one when none is selected) and closes the list, Escape
    // clears the search; both give the keyboard back to the host.
    struct ResultRow
    {
        chipdsp::ChipId chip = chipdsp::ChipId::Nes;
        juce::String name;        // empty: the chip's header row
        juce::String location;    // "Category / Subcategory"
        int count = 0;            // header rows: the chip's number of results
        bool isHeader() const noexcept { return name.isEmpty(); }
    };
    struct RowLayout
    {
        juce::Rectangle<int> tag, name, location;
    };
    static RowLayout rowLayout (int width, int height);
    bool isResultRow (int row) const;
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void selectedRowsChanged (int lastRowSelected) override;
    juce::String getTooltipForRow (int row) override;
    void applyResult (int row);
    int firstResultRow() const;
    bool moveSelection (int direction);
    bool searchKeyPressed (const juce::KeyPress& key);
    void releaseSearchFocus();

    void randomize();
    void updateArpRateControls();
    ParamControl* addControl (FormSection& form, const char* paramId, const juce::String& label, ControlStyle style);

    RetroChipProcessor& processor;
    UiContext& ctx;
    juce::Rectangle<int> header, sidebar;

    // header
    std::unique_ptr<ParamChoice> chipChoice;
    juce::TextButton previousButton { "Previous" }, nextButton { "Next" };
    juce::TextButton presetButton;
    SearchField searchField;
    juce::ListBox resultsList { "Search results", this };
    juce::Component* resultsSummary = nullptr;   // the list's header line (owned by the list)
    std::unique_ptr<Knob> randomAmount;
    juce::TextButton randomizeButton { "Randomize" };
    juce::ComboBox scaleBox;
    juce::TextButton diagnosticsButton { "Diagnostics" };

    // sidebar
    FormSection arpSection { "Arpeggiator" }, glideSection { "Glide" }, outputSection { "Output" };
    std::vector<std::unique_ptr<ParamControl>> sidebarControls;
    ParamControl* arpDivision = nullptr;
    ParamControl* arpFreeRate = nullptr;
    std::unique_ptr<PolyChannelsControl> polyChannels;
    std::unique_ptr<juce::ParameterAttachment> rateModeAttachment;
    juce::Label sampleStatus;   // PresetManager::sampleStatus() under the sidebar forms, hidden when empty
    void refreshSampleStatus();

    // browsing state (what Previous/Next walk through)
    juce::String browseCategory, browseSubcategory;
    std::vector<ResultRow> resultRows;
    int numResults = 0;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CommonStrip)
};

} // namespace rcv
