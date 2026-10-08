#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "ui/ChannelScope.h"
#include "ui/ChipPanel.h"
#include "ui/CommonStrip.h"
#include "ui/DiagnosticsOverlay.h"
#include "ui/ParamControl.h"
#include "ui/RcvLookAndFeel.h"

#include <array>
#include <memory>

namespace rcv
{

// The plugin editor (docs/PLUGIN_SPECS.md "Editor").
//
// Layout: everything lives in one content component laid out at 1280 x 720 logical px
// (ui/EditorLayout.h) and scaled as a whole by ui_scale (1.0..2.0) with an AffineTransform;
// the editor's size is the scaled size. The host or the bottom-right corner can resize the
// editor between 1280 x 720 and 2560 x 1440 with a fixed 16:9 aspect ratio, limited to the
// largest scale that fits the display's work area (UI scale entries beyond it are disabled;
// a stored larger ui_scale is kept but shown at the limit). The new width sets ui_scale,
// written as one gesture once the drag has ended; a change of ui_scale (UI scale box,
// state restore) resizes the editor.
//
// Keyboard: only the preset search field takes keys; while focused it consumes every key
// (CommonStrip::SearchField), so the host does not also play notes from them. A mouse
// press anywhere else in the editor (FocusReleaser, which sees the presses of every
// child) releases the keyboard: no component keeps the focus and, in a host window on
// Windows, the native focus returns to the host (ui/KeyboardFocus.h), so the host keeps
// its shortcuts and computer-keyboard note input. The clicked control, section or the
// content still wants focus for the moment of the click, so JUCE never moves the focus
// to the search field on its own.
//
// Faceplate: the editor itself paints the brushed metal (theme::makeFaceplate()), outside the
// content's scale transform, at the window's size in physical pixels, so the streaks stay
// one device pixel fine at 150 % and 200 %. The image is made once per pixel size and
// cached; the content and the chip panels on top of it are transparent.
//
// Typeface: every open editor holds the optional user typeface (theme::acquireUserTypeface(),
// TypefaceHold, the first member, released last); the last editor closed frees it.
//
// Rendering: a juce::VBlankAttachment drives everything that moves: MIDI learn draining,
// control-state transitions (RcvLookAndFeel), the channel scopes and the diagnostics
// overlay. There is no juce::Timer for painting, and nothing repaints while nothing changes
// (the scopes skip silent or unchanged signals, transitions stop when finished); the
// diagnostics repaint counter reads 0 at rest.
//
// Chip switching: a ParameterAttachment on `chip` (called on the message thread) shows the
// selected chip's panel and retitles the scopes. All three panels are built up front so a
// switch never builds components.
//
// Validation hook (Standalone): when the environment variable RCV_SCREENSHOT holds a PNG
// path, the editor saves createComponentSnapshot() of itself on the third vblank after it
// is showing (layout and first paint done); with RCV_SCREENSHOT_QUIT=1 the standalone app
// then quits. RCV_UI_SCALE overrides ui_scale at start, RCV_SCREENSHOT_CHIP (nes, snes,
// genesis) selects the chip, RCV_SCREENSHOT_DIAGNOSTICS=1 shows the overlay (the snapshot
// then waits 2.5 s so the statistics fill), RCV_SCREENSHOT_SEARCH types into the preset
// search field and RCV_SCREENSHOT_MENU (preset, learn) then opens the preset menu or the
// MIDI learn menu of the panel's first control and saves each menu window as
// "<name>_menu.png" next to the editor snapshot. RCV_SCREENSHOT_PRESET applies a factory
// preset of the selected chip by name; RCV_SCREENSHOT_NOTES=1 queues a held chord (C2 C3 E3
// G3) through RetroChipProcessor::queueTestNotes() and waits 0.5 s, so the channel scopes
// show real waveforms. RCV_SCREENSHOT_FONT=default draws Bahnschrift even when a user
// typeface is installed. An RCV_UI_SCALE larger than the display allows renders the snapshot
// at that scale (the window stays at the limit). None of this runs unless RCV_SCREENSHOT is
// set.
class RetroChipEditor final : public juce::AudioProcessorEditor,
                              private juce::ChangeListener
{
public:
    explicit RetroChipEditor (RetroChipProcessor& processor);
    ~RetroChipEditor() override;

    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void resized() override;
    void parentHierarchyChanged() override;

private:
    // The whole editor at 1280 x 720, transparent over the faceplate.
    class Content final : public juce::Component
    {
    public:
        Content();
    };

    struct TypefaceHold
    {
        TypefaceHold();
        ~TypefaceHold() { theme::releaseUserTypeface(); }
    };

    void onVBlank (double timestampSec);
    void showChip (chipdsp::ChipId chip);
    void applyScale (float scale);
    void commitPendingScale();
    float screenMaxScale() const;
    void updateScreenLimits();
    void setDiagnostics (bool shown);
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void updateSampleList();   // SNES panel: loaded slots and free APU RAM
    void readScreenshotSettings();
    void applyScreenshotSearch();
    void takeScreenshotIfRequested();

    TypefaceHold typefaceHold;    // first: the user typeface outlives every font user
    RcvLookAndFeel lookAndFeel;   // outlives every child
    RetroChipProcessor& rcvProcessor;
    UiContext ctx;

    DiagnosticsOverlay diagnostics;
    Content content;
    std::array<std::unique_ptr<ChipPanel>, ParamRegistry::kNumChips> panels;
    ChannelScope scope;
    CommonStrip strip;
    juce::TooltipWindow tooltips;

    struct FocusReleaser final : juce::MouseListener
    {
        explicit FocusReleaser (RetroChipEditor& o) : owner (o) {}
        void mouseDown (const juce::MouseEvent& e) override;
        RetroChipEditor& owner;
    };
    FocusReleaser focusReleaser { *this };

    std::unique_ptr<juce::ParameterAttachment> chipAttachment;
    std::unique_ptr<juce::ParameterAttachment> scaleAttachment;
    std::unique_ptr<juce::ParameterAttachment> echoDelayAttachment;

    juce::Image faceplate;        // brushed metal at the window's physical pixel size
    double paintStartMs = 0.0;

    float scale = 1.0f;
    float screenMax = 2.0f;            // largest scale that fits the display (theme::kMaxScale at most)
    float pendingScaleCommit = -1.0f;  // ui_scale to write once the resize drag has ended
    bool applyingScale = false;
    bool committingScale = false;
    chipdsp::ChipId shownChip = chipdsp::ChipId::Nes;

    juce::String screenshotPath;
    juce::String screenshotMenu;
    float screenshotScale = 0.0f;   // requested RCV_UI_SCALE (0: none)
    bool screenshotQuit = false;
    bool screenshotNotes = false;
    bool editorShotTaken = false;
    bool screenshotDone = false;
    int shownFrames = 0;
    int menuFrames = 0;
    double shownSinceMs = 0.0;

    juce::VBlankAttachment vblank;   // last: stops before anything it drives is destroyed

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RetroChipEditor)
};

} // namespace rcv
