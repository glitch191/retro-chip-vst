#pragma once

#include "PluginProcessor.h"
#include "ui/Knob.h"
#include "ui/ParamChoice.h"
#include "ui/ParamGroup.h"
#include "ui/ParamToggle.h"

#include <functional>
#include <memory>
#include <vector>

namespace rcv
{

// A titled box of label + control rows (the sidebar forms). Each row holds one control or
// several side by side with equal widths; inline controls share one label column.
class FormSection final : public PanelSection
{
public:
    explicit FormSection (juce::String titleIn) : PanelSection (std::move (titleIn)) {}

    void addRow (std::vector<juce::Component*> items);
    void setLabelWidth (int width);

    int heightForWidth (int width) const override;
    int preferredWidth() const override;

    void paint (juce::Graphics& g) override { paintBox (g); }
    void resized() override;

private:
    std::vector<std::vector<juce::Component*>> rows;
};

// Controls shared by the three chips (docs/PLUGIN_SPECS.md "CommonStrip"):
//
//   header:  chip selector, preset browser (previous / name menu / next, search field with a
//            results list), randomize amount and button, UI scale, Diagnostics toggle;
//   sidebar: arpeggiator (enable, hold, pattern, octaves, rate mode, division or free rate,
//            gate), glide (time, mode) and output (voice mode, raw output, master gain).
//
// The strip covers the whole editor content but only its children take mouse clicks, so the
// chip panel between the header and the sidebar stays reachable. The preset menu is two
// levels deep: category -> subcategory -> preset (presets without a subcategory sit in the
// category menu); choosing a preset makes its subcategory the list Previous/Next walk
// through; typing in the search field lists the matching presets (name and tags) under it
// and makes the matches the Previous/Next list.
class CommonStrip final : public juce::Component,
                          private juce::ChangeListener,
                          private juce::ListBoxModel
{
public:
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

    // Results list (juce::ListBoxModel). Keyboard: Up/Down in the search field move the
    // selection, Return loads the selected match (the first one when none is selected) and
    // closes the list, Escape clears the search; both give the keyboard focus back.
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    juce::String getTooltipForRow (int row) override;
    void applyResult (int row);
    bool searchKeyPressed (const juce::KeyPress& key);
    void releaseSearchFocus();

    struct SearchKeys final : juce::KeyListener
    {
        explicit SearchKeys (CommonStrip& o) : owner (o) {}
        bool keyPressed (const juce::KeyPress& key, juce::Component*) override { return owner.searchKeyPressed (key); }
        CommonStrip& owner;
    };

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
    juce::TextEditor searchField;
    SearchKeys searchKeys { *this };
    juce::ListBox resultsList { "Search results", this };
    std::unique_ptr<Knob> randomAmount;
    juce::TextButton randomizeButton { "Randomize" };
    juce::ComboBox scaleBox;
    juce::TextButton diagnosticsButton { "Diagnostics" };

    // sidebar
    FormSection arpSection { "Arpeggiator" }, glideSection { "Glide" }, outputSection { "Output" };
    std::vector<std::unique_ptr<ParamControl>> sidebarControls;
    ParamControl* arpDivision = nullptr;
    ParamControl* arpFreeRate = nullptr;
    std::unique_ptr<juce::ParameterAttachment> rateModeAttachment;
    juce::Label sampleStatus;   // PresetManager::sampleStatus() under the sidebar forms, hidden when empty
    void refreshSampleStatus();

    // browsing state (what Previous/Next walk through)
    juce::String browseCategory, browseSubcategory;
    std::vector<juce::String> results;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CommonStrip)
};

} // namespace rcv
