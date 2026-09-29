// Host-visible parameter text: every APVTS parameter formats its value through the editor's
// formatter (ValueFormat) and parses that text back. Spot values per chip (PSG attenuation,
// YM2612 TL steps, SNES echo delay, NES duty, Hz and ms parameters) and a round trip over
// every parameter at its minimum, maximum, default and mid-range value.

#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;

namespace
{
    juce::RangedAudioParameter& param (rcv::RetroChipProcessor& proc, const juce::String& id)
    {
        auto* p = proc.parameters().getParameter (id);
        REQUIRE (p != nullptr);
        return *p;
    }

    // Host API: text of an APVTS raw value, and the raw value a text sets.
    juce::String hostText (juce::RangedAudioParameter& p, float raw) { return p.getText (p.convertTo0to1 (raw), 0); }
    float hostValue (juce::RangedAudioParameter& p, const juce::String& text) { return p.convertFrom0to1 (p.getValueForText (text)); }

    struct TextCase
    {
        const char* id;
        float raw;          // APVTS raw value (choice index for choices)
        const char* text;
    };

    void checkCases (rcv::RetroChipProcessor& proc, std::initializer_list<TextCase> cases)
    {
        for (const auto& c : cases)
        {
            INFO ("parameter " << c.id << " at " << c.raw);
            auto& p = param (proc, c.id);
            CHECK (hostText (p, c.raw) == juce::String (c.text));
            const float tolerance = 1.0e-4f * std::max (1.0f, p.convertFrom0to1 (1.0f) - p.convertFrom0to1 (0.0f));
            CHECK (std::abs (hostValue (p, c.text) - c.raw) <= tolerance);
        }
    }
} // namespace

TEST_CASE ("Global parameters show formatted text to the host", "[parameters][text]")
{
    auto proc = rcvtest::makeProcessor();
    checkCases (*proc, {
        { rcv::ParamIds::arpFreeRate, 8.0f,   "8.0 Hz" },
        { rcv::ParamIds::arpFreeRate, 12.5f,  "12.5 Hz" },
        { rcv::ParamIds::glideTime,   250.0f, "250 ms" },
        { rcv::ParamIds::glideTime,   0.0f,   "0 ms" },
        { rcv::ParamIds::masterGain,  -6.0f,  "-6.0 dB" },
        { rcv::ParamIds::masterGain,  3.5f,   "+3.5 dB" },
        { rcv::ParamIds::arpGate,     50.0f,  "50 %" },
        { rcv::ParamIds::voiceMode,   0.0f,   "MIDI channel" },
        { rcv::ParamIds::arpSyncDivision, 2.0f, "1/8T" },
        { rcv::ParamIds::arpEnabled,  1.0f,   "On" },
        { rcv::ParamIds::arpOctaves,  3.0f,   "3" },
    });

    // Plain numbers and other spellings are accepted too.
    CHECK_THAT (hostValue (param (*proc, rcv::ParamIds::glideTime), "120"), WithinAbs (120.0, 0.01));
    CHECK_THAT (hostValue (param (*proc, rcv::ParamIds::masterGain), "+2 dB"), WithinAbs (2.0, 0.001));
    CHECK (hostValue (param (*proc, rcv::ParamIds::voiceMode), "poly") == 1.0f);
    CHECK (hostValue (param (*proc, rcv::ParamIds::arpEnabled), "off") == 0.0f);
}

TEST_CASE ("Engine parameters show hardware units converted, not register text", "[parameters][text][real]")
{
    auto proc = rcvtest::makeProcessor();
    if (rcvtest::usesStubEngines (*proc))
        SKIP ("Stub engines: no hardware parameters");

    checkCases (*proc, {
        // NES: duty choice labels, integer registers.
        { "nes_p1_duty", 2.0f, "50 %" },
        { "nes_p1_duty", 0.0f, "12.5 %" },
        // SNES: echo delay in ms (EDL x 16 ms, EDL 0 = one sample).
        { "snes_echo_delay", 3.0f,  "48 ms" },
        { "snes_echo_delay", 15.0f, "240 ms" },
        { "snes_echo_delay", 0.0f,  "0.03 ms" },
        // Genesis: SN76489 attenuation (2 dB steps, 15 = off), YM2612 TL steps (0.75 dB), Hz.
        { "genesis_psg1_att", 3.0f,  "-6 dB" },
        { "genesis_psg1_att", 0.0f,  "0 dB" },
        { "genesis_psg1_att", 15.0f, "Off" },
        { "genesis_velocity_depth", 24.0f, "-18 dB" },
        { "genesis_velocity_depth", 1.0f,  "-0.75 dB" },
        { "genesis_dac_rate", 16000.0f, "16000 Hz" },
        { "genesis_psg_transpose", 12.0f, "+12 st" },
        { "genesis_psg_transpose", -7.0f, "-7 st" },
        { "genesis_psg_sw_attack", 1.0f, "1 frame" },
    });

    // Typed values in the displayed unit.
    CHECK (hostValue (param (*proc, "genesis_psg1_att"), "-6") == 3.0f);
    CHECK (hostValue (param (*proc, "genesis_psg1_att"), "off") == 15.0f);
    CHECK (hostValue (param (*proc, "snes_echo_delay"), "96") == 6.0f);
    CHECK (hostValue (param (*proc, "genesis_velocity_depth"), "-3 dB") == 4.0f);

    // No register unit leaks into the host text.
    for (const auto& info : proc->paramRegistry().all())
    {
        auto& p = param (*proc, info.id);
        for (float raw : { p.convertFrom0to1 (0.0f), p.convertFrom0to1 (0.5f), p.convertFrom0to1 (1.0f) })
        {
            const auto text = hostText (p, raw);
            INFO ("parameter " << info.id << ": '" << text << "'");
            CHECK_FALSE (text.contains ("x2 dB"));
            CHECK_FALSE (text.endsWith (" TL"));
            CHECK_FALSE (text.endsWith (" fnum"));
            CHECK_FALSE (text.endsWith (" period"));
        }
    }
}

TEST_CASE ("Every parameter parses back the text it shows", "[parameters][text]")
{
    auto proc = rcvtest::makeProcessor();
    int checks = 0;
    for (const auto& info : proc->paramRegistry().all())
    {
        auto& p = param (*proc, info.id);
        const float lo = p.convertFrom0to1 (0.0f);
        const float hi = p.convertFrom0to1 (1.0f);
        // A float's text is rounded to its display precision (0 to 2 decimals).
        const float tolerance = info.kind == rcv::ParamKind::Float ? 0.006f * std::max (1.0f, hi - lo) : 0.0f;
        for (float raw : { lo, hi, p.convertFrom0to1 (p.getDefaultValue()), p.convertFrom0to1 (0.37f) })
        {
            const auto text = hostText (p, raw);
            const float back = hostValue (p, text);
            INFO ("parameter " << info.id << ": " << raw << " -> '" << text << "' -> " << back);
            CHECK (std::abs (back - raw) <= tolerance);
            CHECK (hostText (p, back) == text);
            ++checks;
        }
    }
    CHECK (checks > 100);
}
