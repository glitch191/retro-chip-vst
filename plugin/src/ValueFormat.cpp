#include "ValueFormat.h"

#include <cmath>

namespace rcv
{

namespace
{
    constexpr float kPsgDbPerStep = 2.0f;     // SN76489 attenuation step; 15 = off
    constexpr int kPsgOffStep = 15;
    constexpr float kTlDbPerStep = 0.75f;     // YM2612 total level step
    constexpr float kEchoMsPerStep = 16.0f;   // SNES EDL: 512 stereo samples at 32 kHz per step
    constexpr float kEchoMinMs = 1000.0f / 32000.0f;   // EDL 0: a 4-byte buffer, one sample

    juce::String unitOf (const ParamInfo& info)
    {
        return info.desc.unit != nullptr ? juce::String (info.desc.unit) : juce::String();
    }

    bool isSnesEchoDelay (const ParamInfo& info)
    {
        return info.chip == chipdsp::ChipId::Snes && info.engineKey() == "echo_delay";
    }

    int choiceCount (const chipdsp::ParamDesc& d)
    {
        return static_cast<int> (std::lround (d.maxValue - d.minValue)) + 1;
    }

    juce::String trimNumber (double value, int decimals)
    {
        auto text = juce::String (value, decimals);
        if (text.containsChar ('.'))
            text = text.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
        return text;
    }

    // The leading number of the text ("+12 st" -> 12, "-4.5 dB" -> -4.5); false when there is none.
    bool readNumber (const juce::String& text, float& out)
    {
        auto t = text.trim();
        if (t.startsWithChar ('+'))
            t = t.substring (1).trimStart();
        if (! t.containsAnyOf ("0123456789"))
            return false;
        out = t.getFloatValue();
        return std::isfinite (out);
    }
} // namespace

juce::String formatNativeValue (const ParamInfo& info, float native)
{
    const auto& d = info.desc;
    if (d.choiceLabels != nullptr)
    {
        const int index = juce::jlimit (0, choiceCount (d) - 1, static_cast<int> (std::lround (native - d.minValue)));
        return juce::String (d.choiceLabels[index]);
    }
    if (info.kind == ParamKind::Bool)
        return native > 0.5f ? "On" : "Off";

    const juce::String unit = unitOf (info);
    const int steps = static_cast<int> (std::lround (native));
    if (unit == "x2 dB")
        return steps >= kPsgOffStep ? juce::String ("Off")
                                    : (steps <= 0 ? juce::String ("0 dB") : "-" + juce::String (static_cast<int> (kPsgDbPerStep) * steps) + " dB");
    if (unit == "TL")
        return steps <= 0 ? juce::String ("0 dB") : "-" + trimNumber (kTlDbPerStep * steps, 2) + " dB";
    if (isSnesEchoDelay (info))
        return steps <= 0 ? trimNumber (kEchoMinMs, 2) + " ms" : juce::String (static_cast<int> (kEchoMsPerStep) * steps) + " ms";

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

float parseNativeValue (const ParamInfo& info, const juce::String& text)
{
    const auto& d = info.desc;
    const auto t = text.trim();

    if (d.choiceLabels != nullptr)
        for (int i = 0; i < choiceCount (d); ++i)
            if (t.equalsIgnoreCase (d.choiceLabels[i]))
                return d.minValue + static_cast<float> (i);

    if (info.kind == ParamKind::Bool)
    {
        if (t.equalsIgnoreCase ("on") || t.equalsIgnoreCase ("true") || t.equalsIgnoreCase ("yes"))
            return 1.0f;
        if (t.equalsIgnoreCase ("off") || t.equalsIgnoreCase ("false") || t.equalsIgnoreCase ("no"))
            return 0.0f;
    }

    const juce::String unit = unitOf (info);
    if (unit == "x2 dB" && t.equalsIgnoreCase ("off"))
        return static_cast<float> (kPsgOffStep);

    float number = 0.0f;
    if (! readNumber (t, number))
        return info.clampNative (d.defaultValue);

    // Converted units: the text is in dB (attenuation, sign optional) or ms.
    if (unit == "x2 dB")
        return info.clampNative (std::round (std::abs (number) / kPsgDbPerStep));
    if (unit == "TL")
        return info.clampNative (std::round (std::abs (number) / kTlDbPerStep));
    if (isSnesEchoDelay (info))
        return info.clampNative (std::round (number / kEchoMsPerStep));

    return info.clampNative (number);
}

} // namespace rcv
