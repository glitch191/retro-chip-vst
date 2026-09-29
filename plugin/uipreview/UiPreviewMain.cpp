// ui_preview: renders the three chip panels offscreen to PNG files with the parameter sets
// the real engines will publish (tools/presetgen/params/<chip>.json, transcribed from
// docs/ENGINE_SPECS.md), so the panel layouts can be checked for overflow, truncation and
// overlap while the plugin still runs the placeholder engines.
//
//   ui_preview <output dir> [scale]
//
// Writes <output dir>/panel_<chip>.png (the panel area of the editor at the given scale)
// and prints the number of pixels by which each layout overflows the panel area.
// Development tool only (CMake option RCV_BUILD_UI_PREVIEW, off by default).

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "MidiLearn.h"
#include "Parameters.h"
#include "ui/EditorLayout.h"
#include "ui/GenesisPanel.h"
#include "ui/NesPanel.h"
#include "ui/RcvLookAndFeel.h"
#include "ui/SnesPanel.h"
#include "ui/Theme.h"

#include "chipdsp/EngineFactory.h"

#include <deque>
#include <iostream>
#include <string>

namespace
{

// An engine that only publishes parameter descriptors read from a JSON table.
class DescriptorEngine final : public chipdsp::IChipEngine
{
public:
    DescriptorEngine (chipdsp::ChipId c, const juce::var& table) : chip (c)
    {
        if (auto* list = table["params"].getArray())
        {
            for (const auto& p : *list)
            {
                chipdsp::ParamDesc d {};
                d.id = static_cast<int> (p["id"]);
                d.key = keep (p["key"].toString());
                d.name = keep (p["name"].toString());
                d.group = keep (p["group"].toString());
                d.minValue = static_cast<float> (p["min"]);
                d.maxValue = static_cast<float> (p["max"]);
                d.defaultValue = static_cast<float> (p["default"]);
                d.isInteger = static_cast<bool> (p["isInteger"]);
                d.unit = keep (p["unit"].toString());
                d.choiceLabels = nullptr;
                if (auto* labels = p["labels"].getArray())
                {
                    std::vector<const char*> arr;
                    for (const auto& l : *labels)
                        arr.push_back (keep (l.toString()));
                    labelArrays.push_back (std::move (arr));
                    d.choiceLabels = labelArrays.back().data();
                }
                descs.push_back (d);
            }
        }
    }

    chipdsp::ChipId chipId() const noexcept override { return chip; }
    int numChannels() const noexcept override { return chipdsp::chipChannelCount (chip); }
    chipdsp::ChannelInfo channelInfo (int) const noexcept override { return { "Channel", "C", true }; }
    double nativeSampleRate() const noexcept override { return 48000.0; }
    void prepare (double, int) override {}
    std::span<const chipdsp::ParamDesc> parameterDescriptors() const noexcept override { return { descs.data(), descs.size() }; }
    bool loadSample (int, const float*, int, double) override { return false; }
    int numSampleSlots() const noexcept override { return 0; }
    void reset() noexcept override {}
    void setParameter (int, float) noexcept override {}
    float getParameter (int) const noexcept override { return 0.0f; }
    void noteOn (int, float, float) noexcept override {}
    void noteOff (int) noexcept override {}
    void setChannelPitch (int, float) noexcept override {}
    bool isChannelActive (int) const noexcept override { return false; }
    void setClockStandard (chipdsp::ClockStandard) noexcept override {}
    void setRawOutput (bool) noexcept override {}
    void renderBlock (float*, float*, float* const*, float* const*, int) noexcept override {}

private:
    const char* keep (const juce::String& s)
    {
        strings.push_back (s.toStdString());
        return strings.back().c_str();
    }

    chipdsp::ChipId chip;
    std::deque<std::string> strings;
    std::deque<std::vector<const char*>> labelArrays;
    std::vector<chipdsp::ParamDesc> descs;
};

class PreviewProcessor final : public juce::AudioProcessor
{
public:
    explicit PreviewProcessor (const rcv::ParamRegistry& registry)
        : apvts (*this, nullptr, "Parameters", registry.createParameterLayout())
    {
    }

    const juce::String getName() const override { return "ui_preview"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    juce::AudioProcessorValueTreeState apvts;
};

juce::var loadTable (const char* chipKey)
{
    const juce::File file = juce::File (RCV_PARAMS_DIR).getChildFile (juce::String (chipKey) + ".json");
    return juce::JSON::parse (file);
}

} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;

    const juce::File outDir = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1])
                                       : juce::File::getCurrentWorkingDirectory();
    const float scale = argc > 2 ? juce::String (argv[2]).getFloatValue() : 1.0f;
    outDir.createDirectory();

    DescriptorEngine nes (chipdsp::ChipId::Nes, loadTable ("nes"));
    DescriptorEngine snes (chipdsp::ChipId::Snes, loadTable ("snes"));
    DescriptorEngine genesis (chipdsp::ChipId::Genesis, loadTable ("genesis"));
    rcv::ParamRegistry registry ({ &nes, &snes, &genesis });
    PreviewProcessor processor (registry);
    rcv::MidiLearn learn (processor.apvts);

    int exitCode = 0;
    {
        rcv::RcvLookAndFeel lnf;
        juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
        rcv::UiContext ctx { processor.apvts, registry, learn };

        const auto area = rcv::layout::panel();
        for (int c = 0; c < 3; ++c)
        {
            std::unique_ptr<rcv::ChipPanel> panel;
            if (c == 0)
                panel = std::make_unique<rcv::NesPanel> (ctx);
            else if (c == 1)
                panel = std::make_unique<rcv::SnesPanel> (ctx);
            else
                panel = std::make_unique<rcv::GenesisPanel> (ctx);
            panel->setBounds (0, 0, area.getWidth(), area.getHeight());

            juce::Image image (juce::Image::ARGB, juce::roundToInt (static_cast<float> (area.getWidth()) * scale),
                               juce::roundToInt (static_cast<float> (area.getHeight()) * scale), true);
            {
                juce::Graphics g (image);
                g.fillAll (rcv::theme::colours::background);
                g.addTransform (juce::AffineTransform::scale (scale));
                panel->paintEntireComponent (g, false);
            }

            const auto key = juce::String (chipdsp::chipKey (static_cast<chipdsp::ChipId> (c)));
            const auto file = outDir.getChildFile ("panel_" + key + ".png");
            file.deleteFile();
            juce::FileOutputStream stream (file);
            juce::PNGImageFormat png;
            if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
                exitCode = 1;
            std::cout << key << ": params " << registry.engineParams (static_cast<chipdsp::ChipId> (c)).size()
                      << ", overflow " << panel->overflow() << " px -> " << file.getFullPathName() << std::endl;
            if (panel->overflow() > 0)
                exitCode = 2;
        }
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }
    return exitCode;
}
