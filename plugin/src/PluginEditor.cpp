#include "PluginEditor.h"

#include "ui/EditorLayout.h"
#include "ui/GenesisPanel.h"
#include "ui/NesPanel.h"
#include "ui/SnesPanel.h"
#include "ui/Theme.h"

#include <cmath>

namespace rcv
{

namespace
{
    constexpr int kScreenshotFrames = 3;
    constexpr double kScreenshotDiagnosticsDelayMs = 2500.0;   // let the 2 s statistics fill
    constexpr double kScreenshotNotesDelayMs = 500.0;          // let the chord reach the scopes

    juce::String env (const char* name)
    {
        return juce::SystemStats::getEnvironmentVariable (name, {}).trim();
    }
} // namespace

// ----- Content ----------------------------------------------------------------------------------

RetroChipEditor::Content::Content (DiagnosticsOverlay& overlay) : diagnostics (overlay)
{
    setOpaque (true);
    // Clicks on empty areas focus the content (it handles no keys, so keys reach the host).
    setWantsKeyboardFocus (true);
}

void RetroChipEditor::Content::paint (juce::Graphics& g)
{
    paintStartMs = juce::Time::getMillisecondCounterHiRes();
    g.fillAll (theme::colours::background);

    // Every paint of the editor passes through here (the content is the opaque bottom
    // layer); paints confined to the opaque overlay itself are not counted.
    if (diagnostics.isActive() && ! diagnostics.getBounds().expanded (theme::kOpticalOffset).contains (g.getClipBounds()))
        diagnostics.countRepaint();
}

void RetroChipEditor::Content::paintOverChildren (juce::Graphics&)
{
    // paint() .. paintOverChildren() brackets the whole editor paint (children included).
    // JUCE skips paint() but still calls paintOverChildren() when the dirty area is covered
    // by an opaque child (the overlay alone): nothing is measured then.
    if (paintStartMs > 0.0)
        diagnostics.addPaintTime (juce::Time::getMillisecondCounterHiRes() - paintStartMs);
    paintStartMs = 0.0;
}

// ----- RetroChipEditor --------------------------------------------------------------------------

RetroChipEditor::RetroChipEditor (RetroChipProcessor& p)
    : AudioProcessorEditor (p),
      rcvProcessor (p),
      ctx { p.parameters(), p.paramRegistry(), p.midiLearn() },
      scope (p.visualizer()),
      strip (p, ctx),
      tooltips (&content, theme::kTooltipDelayMs),
      vblank (this, [this] (double timestampSec) { onVBlank (timestampSec); })
{
    setLookAndFeel (&lookAndFeel);
    setOpaque (true);

    content.setBounds (0, 0, theme::kBaseWidth, theme::kBaseHeight);
    addAndMakeVisible (content);

    panels[0] = std::make_unique<NesPanel> (ctx);
    panels[1] = std::make_unique<SnesPanel> (ctx);
    panels[2] = std::make_unique<GenesisPanel> (ctx);
    for (auto& panel : panels)
    {
        content.addChildComponent (*panel);
        panel->setBounds (layout::panel());
    }

    content.addAndMakeVisible (scope);
    scope.setBounds (layout::scopes());

    content.addAndMakeVisible (strip);
    strip.setBounds (content.getLocalBounds());
    strip.setAreas (layout::header(), layout::sidebar());
    strip.onScaleChosen = [this] (float s)
    {
        pendingScaleCommit = -1.0f;
        if (scaleAttachment != nullptr)
            scaleAttachment->setValueAsCompleteGesture (s);
        applyScale (s);
    };
    strip.onDiagnosticsToggled = [this] (bool shown) { setDiagnostics (shown); };

    const auto panelArea = layout::panel();
    content.addChildComponent (diagnostics);
    diagnostics.setBounds (panelArea.getRight() - theme::kGap - DiagnosticsOverlay::preferredWidth(),
                           panelArea.getBottom() - theme::kGap - DiagnosticsOverlay::preferredHeight(),
                           DiagnosticsOverlay::preferredWidth(), DiagnosticsOverlay::preferredHeight());

    rcvProcessor.visualizer().setEnabled (true);
    rcvProcessor.midiLearn().addChangeListener (this);

    auto& apvts = rcvProcessor.parameters();
    if (auto* chipParam = apvts.getParameter (ParamIds::chip))
    {
        chipAttachment = std::make_unique<juce::ParameterAttachment> (*chipParam, [this] (float value)
        {
            showChip (static_cast<chipdsp::ChipId> (juce::jlimit (0, ParamRegistry::kNumChips - 1, juce::roundToInt (value))));
        });
    }
    if (auto* scaleParam = apvts.getParameter (ParamIds::uiScale))
        scaleAttachment = std::make_unique<juce::ParameterAttachment> (*scaleParam, [this] (float value)
        {
            if (! committingScale)
                applyScale (value);
        });

    // The corner and the host may resize between 1x and the largest scale that fits the
    // screen (at most 2x); the aspect ratio is fixed.
    setResizable (true, true);
    if (auto* sizeConstrainer = getConstrainer())
        sizeConstrainer->setFixedAspectRatio (static_cast<double> (theme::kBaseWidth) / static_cast<double> (theme::kBaseHeight));
    updateScreenLimits();

    readScreenshotSettings();

    showChip (rcvProcessor.selectedChip());
    float initialScale = 1.0f;
    if (auto* raw = apvts.getRawParameterValue (ParamIds::uiScale))
        initialScale = raw->load();
    if (const auto overrideScale = env ("RCV_UI_SCALE"); overrideScale.isNotEmpty())
    {
        initialScale = juce::jlimit (theme::kMinScale, theme::kMaxScale, overrideScale.getFloatValue());
        if (scaleAttachment != nullptr)
            scaleAttachment->setValueAsCompleteGesture (initialScale);
    }
    scale = 0.0f;   // force applyScale to set the transform and the size
    applyScale (initialScale);
    applyScreenshotSearch();
}

RetroChipEditor::~RetroChipEditor()
{
    rcvProcessor.midiLearn().removeChangeListener (this);
    rcvProcessor.visualizer().setEnabled (false);
    setLookAndFeel (nullptr);
}

void RetroChipEditor::paint (juce::Graphics& g)
{
    // Only visible if the host gives a size slightly off the 16:9 ratio.
    g.fillAll (theme::colours::background);
}

void RetroChipEditor::resized()
{
    if (applyingScale || getWidth() <= 0)
        return;

    // Resized by the host or the corner: the width decides the scale. ui_scale is written
    // once, from the vblank callback, after the drag has ended (one gesture per resize).
    const float s = juce::jlimit (theme::kMinScale, theme::kMaxScale, static_cast<float> (getWidth()) / static_cast<float> (theme::kBaseWidth));
    if (std::abs (s - scale) < 0.0005f)
        return;
    scale = s;
    content.setTransform (juce::AffineTransform::scale (s));
    strip.setDisplayedScale (s);
    pendingScaleCommit = s;
}

void RetroChipEditor::commitPendingScale()
{
    if (pendingScaleCommit < 0.0f || juce::Desktop::getInstance().getMainMouseSource().isDragging())
        return;
    const float s = pendingScaleCommit;
    pendingScaleCommit = -1.0f;
    if (scaleAttachment != nullptr)
    {
        const juce::ScopedValueSetter<bool> guard (committingScale, true);
        scaleAttachment->setValueAsCompleteGesture (s);
    }
}

float RetroChipEditor::screenMaxScale() const
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto* display = isShowing() ? displays.getDisplayForRect (getScreenBounds()) : displays.getPrimaryDisplay();
    if (display == nullptr)
        return theme::kMaxScale;
    const auto area = display->userBounds;
    const float fit = juce::jmin (area.getWidth() / static_cast<float> (theme::kBaseWidth),
                                  (area.getHeight() - static_cast<float> (theme::kHostWindowAllowance)) / static_cast<float> (theme::kBaseHeight));
    return juce::jlimit (theme::kMinScale, theme::kMaxScale, fit);
}

void RetroChipEditor::updateScreenLimits()
{
    const float maxScale = screenMaxScale();
    if (std::abs (maxScale - screenMax) < 0.0005f)
        return;
    screenMax = maxScale;
    setResizeLimits (theme::kBaseWidth, theme::kBaseHeight,
                     juce::roundToInt (static_cast<float> (theme::kBaseWidth) * maxScale),
                     juce::roundToInt (static_cast<float> (theme::kBaseHeight) * maxScale));
    strip.setMaxScale (maxScale);
    if (scale > maxScale + 0.0005f)
        applyScale (scale);
}

void RetroChipEditor::parentHierarchyChanged()
{
    updateScreenLimits();
}

void RetroChipEditor::applyScale (float newScale)
{
    // The stored ui_scale is kept; only the shown size is limited to the screen.
    const float s = juce::jlimit (theme::kMinScale, juce::jmin (theme::kMaxScale, screenMax), newScale);
    const int w = juce::roundToInt (static_cast<float> (theme::kBaseWidth) * s);
    const int h = juce::roundToInt (static_cast<float> (theme::kBaseHeight) * s);
    const bool sizeChanges = w != getWidth() || h != getHeight();
    if (std::abs (s - scale) < 0.0005f && ! sizeChanges)
        return;

    scale = s;
    content.setTransform (juce::AffineTransform::scale (s));
    strip.setDisplayedScale (s);
    if (sizeChanges)
    {
        const juce::ScopedValueSetter<bool> guard (applyingScale, true);
        setSize (w, h);
    }
}

void RetroChipEditor::showChip (chipdsp::ChipId chip)
{
    // Applying a preset rewrites `chip` with the same value; only a real change counts.
    if (chip == shownChip && panels[static_cast<size_t> (chip)]->isVisible())
        return;
    shownChip = chip;
    for (size_t i = 0; i < panels.size(); ++i)
        panels[i]->setVisible (static_cast<int> (i) == static_cast<int> (chip));
    scope.setChip (rcvProcessor.engineHost().engine (chip));
    strip.chipChanged();
}

void RetroChipEditor::setDiagnostics (bool shown)
{
    diagnostics.setActive (shown);
    strip.setDiagnosticsShown (shown);
}

void RetroChipEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // MIDI learn armed, learned or cleared: controls redraw their learn outline and tooltip.
    content.repaint();
}

void RetroChipEditor::onVBlank (double timestampSec)
{
    const double startMs = juce::Time::getMillisecondCounterHiRes();
    rcvProcessor.midiLearn().drain();
    lookAndFeel.updateTransitions();
    scope.update();
    commitPendingScale();
    diagnostics.frame (timestampSec, juce::Time::getMillisecondCounterHiRes() - startMs);
    takeScreenshotIfRequested();
}

// ----- screenshot hook --------------------------------------------------------------------------

void RetroChipEditor::readScreenshotSettings()
{
    screenshotPath = env ("RCV_SCREENSHOT");
    if (screenshotPath.isEmpty())
        return;
    screenshotQuit = env ("RCV_SCREENSHOT_QUIT") == "1";
    screenshotMenu = env ("RCV_SCREENSHOT_MENU").toLowerCase();

    const auto chipName = env ("RCV_SCREENSHOT_CHIP").toLowerCase();
    const int chipIndex = chipName == "snes" ? 1 : (chipName == "genesis" ? 2 : (chipName == "nes" ? 0 : -1));
    if (chipIndex >= 0)
        if (auto* chipParam = rcvProcessor.parameters().getParameter (ParamIds::chip))
            chipParam->setValueNotifyingHost (chipParam->convertTo0to1 (static_cast<float> (chipIndex)));

    // RCV_SCREENSHOT_PRESET: a factory preset of the selected chip, by name.
    auto& pm = rcvProcessor.presetManager();
    if (const auto presetName = env ("RCV_SCREENSHOT_PRESET"); presetName.isNotEmpty())
    {
        if (const auto* preset = pm.findByName (rcvProcessor.selectedChip(), presetName))
            pm.apply (*preset);
        else
            juce::Logger::writeToLog ("RCV_SCREENSHOT_PRESET: no preset '" + presetName + "'");
    }

    // RCV_SCREENSHOT_NOTES=1: a held four-note chord, so the channel scopes show the chip's
    // real waveforms in the snapshot (the audio device must be running).
    screenshotNotes = env ("RCV_SCREENSHOT_NOTES") == "1";
    if (screenshotNotes)
        rcvProcessor.queueTestNotes ({ 48, 60, 64, 67 }, 100);

    if (env ("RCV_SCREENSHOT_DIAGNOSTICS") == "1")
        setDiagnostics (true);
}

void RetroChipEditor::applyScreenshotSearch()
{
    if (screenshotPath.isNotEmpty())
        if (const auto text = env ("RCV_SCREENSHOT_SEARCH"); text.isNotEmpty())
            strip.setSearchText (text);
}

namespace
{
    void writePng (const juce::Image& image, const juce::File& file)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::FileOutputStream stream (file);
        juce::PNGImageFormat png;
        if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
            juce::Logger::writeToLog ("RCV_SCREENSHOT: could not write " + file.getFullPathName());
    }

    ParamControl* firstParamControl (juce::Component& root)
    {
        for (auto* child : root.getChildren())
        {
            if (auto* control = dynamic_cast<ParamControl*> (child); control != nullptr && control->paramInfo() != nullptr)
                return control;
            if (auto* found = firstParamControl (*child))
                return found;
        }
        return nullptr;
    }
} // namespace

void RetroChipEditor::takeScreenshotIfRequested()
{
    if (screenshotPath.isEmpty() || screenshotDone || ! isShowing())
        return;
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (shownFrames++ == 0)
        shownSinceMs = now;
    if (shownFrames < kScreenshotFrames)
        return;
    if (diagnostics.isActive() && now - shownSinceMs < kScreenshotDiagnosticsDelayMs)
        return;
    if (screenshotNotes && now - shownSinceMs < kScreenshotNotesDelayMs)
        return;

    const juce::File file (screenshotPath);
    if (! editorShotTaken)
    {
        editorShotTaken = true;
        writePng (createComponentSnapshot (getLocalBounds(), true, 1.0f), file);

        // Menus are separate desktop windows: open the requested one, capture it a few
        // frames later into "<name>_menu.png".
        if (screenshotMenu == "preset")
            strip.openPresetMenu();
        else if (screenshotMenu == "learn")
            if (auto* control = firstParamControl (*panels[static_cast<size_t> (shownChip)]))
                control->openMidiLearnMenu();
        menuFrames = 0;
        if (screenshotMenu.isNotEmpty())
            return;
    }
    else if (screenshotMenu.isNotEmpty())
    {
        if (++menuFrames < kScreenshotFrames * 4)
            return;
        auto* top = getTopLevelComponent();
        int n = 0;
        for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
        {
            auto* window = juce::Desktop::getInstance().getComponent (i);
            // Menu windows only: not the editor, tooltips or tiny helper windows.
            if (window == nullptr || window == top || ! window->isVisible() || dynamic_cast<juce::TooltipWindow*> (window) != nullptr
                || window->getWidth() < theme::kMinTarget || window->getHeight() < theme::kMinTarget)
                continue;
            const auto name = file.getFileNameWithoutExtension() + "_menu" + (n == 0 ? juce::String() : juce::String (n)) + ".png";
            writePng (window->createComponentSnapshot (window->getLocalBounds(), true, 1.0f), file.getSiblingFile (name));
            ++n;
        }
        if (n == 0)
            juce::Logger::writeToLog ("RCV_SCREENSHOT_MENU: no menu window found");
        juce::PopupMenu::dismissAllActiveMenus();
    }

    screenshotDone = true;
    if (screenshotQuit && juce::JUCEApplicationBase::isStandaloneApp())
        juce::JUCEApplicationBase::quit();
}

} // namespace rcv
