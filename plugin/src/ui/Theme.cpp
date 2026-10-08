#include "ui/Theme.h"

#include <cmath>
#include <vector>

namespace rcv::theme
{

namespace
{
    constexpr const char* kTypeface = "Bahnschrift";

    struct UserTypeface
    {
        juce::Typeface::Ptr face;
        int users = 0;
        bool allowed = true;
    };

    // A plain struct: the Ptr is cleared by the last editor closed, so nothing of JUCE's font
    // system is left for the static destructor.
    UserTypeface& userTypeface()
    {
        static UserTypeface state;
        return state;
    }

    juce::Font withUserTypeface (float size)
    {
        return juce::Font (juce::FontOptions (userTypeface().face).withPointHeight (size * kUserTypefaceScale));
    }
} // namespace

const juce::String& fontFamily()
{
    static const juce::String family = []
    {
        const auto names = juce::Font::findAllTypefaceNames();
        return names.contains (kTypeface) ? juce::String (kTypeface) : juce::Font::getDefaultSansSerifFontName();
    }();
    return family;
}

juce::Font layoutFont (float size, bool bold)
{
    return juce::Font (juce::FontOptions (fontFamily(), bold ? "SemiBold" : "Regular", size).withPointHeight (size));
}

juce::Font layoutSilkFont (float size, float spacing)
{
    return juce::Font (juce::FontOptions (fontFamily(), "SemiBold SemiCondensed", size).withPointHeight (size))
        .withExtraKerningFactor (spacing);
}

juce::Font font (float size, bool bold)
{
    if (userTypeface().face != nullptr)
        return withUserTypeface (size);
    return layoutFont (size, bold);
}

juce::Font silkFont (float size, float spacing)
{
    if (userTypeface().face != nullptr)
        return withUserTypeface (size).withExtraKerningFactor (spacing * 0.6f);
    return layoutSilkFont (size, spacing);
}

void acquireUserTypeface()
{
    auto& state = userTypeface();
    ++state.users;
    if (! state.allowed || state.face != nullptr)
        return;
    const auto folder = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("retro-chip-vst").getChildFile ("fonts");
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.ttf;*.otf");
    files.sort();
    for (const auto& file : files)
    {
        juce::MemoryBlock data;
        if (file.loadFileAsData (data))
            if ((state.face = juce::Typeface::createSystemTypefaceFor (data.getData(), data.getSize())) != nullptr)
                return;
    }
}

void releaseUserTypeface()
{
    auto& state = userTypeface();
    if (--state.users <= 0)
    {
        state.users = 0;
        state.face = nullptr;
    }
}

void setUserTypefaceAllowed (bool allowed)
{
    userTypeface().allowed = allowed;
    if (! allowed)
        userTypeface().face = nullptr;
}

// ----- faceplate drawing --------------------------------------------------------------------

void drawBracket (juce::Graphics& g, float left, float right, float lineY)
{
    g.setColour (colours::silk.withAlpha (0.75f));
    g.fillRect (left, lineY, right - left, kBorder);
    g.fillRect (left, lineY, kBorder, kBracketTick);
    g.fillRect (right - kBorder, lineY, kBorder, kBracketTick);
}

void drawSectionTitle (juce::Graphics& g, const juce::String& title, juce::Rectangle<int> area)
{
    // Title text on the top 18 px, the bracket line under it; the ticks reach into the content.
    const auto line = static_cast<float> (area.getY() + kGroupTitleHeight - kGap + kOpticalOffset);
    g.setColour (colours::silk);
    g.setFont (silkFont (kFontGroup));
    g.drawFittedText (title.toUpperCase(), area.withHeight (kGroupTitleHeight - kGap + kOpticalOffset).withTrimmedLeft (kOpticalOffset),
                      juce::Justification::centredLeft, 1, kMinHorizontalScale);
    drawBracket (g, static_cast<float> (area.getX()), static_cast<float> (area.getRight()), line);
}

void drawRecess (juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour (colours::recess);
    g.fillRoundedRectangle (area, kRadius);
    g.setColour (juce::Colours::black);
    g.drawRoundedRectangle (area.reduced (0.5f * kBorder), kRadius, kBorder);
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.fillRect (area.getX() + 1.0f, area.getBottom() - kBorder, area.getWidth() - 2.0f, kBorder);
}

void drawButtonFace (juce::Graphics& g, juce::Rectangle<float> area, bool enabled, float hover, bool down)
{
    auto top = colours::controlTop, bottom = colours::control;
    if (! enabled)
        top = bottom = colours::faceplate;
    else if (down)
        std::swap (top, bottom);
    else
    {
        top = top.brighter (0.06f * hover);
        bottom = bottom.brighter (0.06f * hover);
    }
    g.setGradientFill (juce::ColourGradient (top, 0.0f, area.getY(), bottom, 0.0f, area.getBottom(), false));
    g.fillRoundedRectangle (area, kRadius);
    g.setColour (colours::controlEdge);
    g.drawRoundedRectangle (area, kRadius, kBorder);
    g.setColour (juce::Colours::white.withAlpha (enabled ? 0.08f : 0.03f));
    g.fillRect (area.getX() + 2.0f, area.getY() + 1.0f, area.getWidth() - 4.0f, kBorder);
}

void drawLed (juce::Graphics& g, juce::Rectangle<float> buttonArea, float on)
{
    const juce::Rectangle<float> led (buttonArea.getX() + kLedInsetX, buttonArea.getY() + kLedInsetY, kLedWidth, kLedHeight);
    if (on > 0.0f)
    {
        g.setColour (colours::ledOn.withAlpha (0.25f * on));
        g.fillRoundedRectangle (led.expanded (kLedGlow), kLedGlow);
    }
    g.setColour (colours::ledOff.interpolatedWith (colours::ledOn, on));
    g.fillRoundedRectangle (led, 1.0f);
}

juce::Image makeFaceplate (int width, int height)
{
    // Dark brushed metal: fine horizontal streaks of varying length on a dark grey with a
    // slight blue tint, lit by two broad soft highlights (upper left +55 %, centre right
    // +47 %). The brightest point stays at or below #434346, the reference of the contrast
    // figures in Theme.h.
    width = std::max (1, width);
    height = std::max (1, height);
    juce::Image image (juce::Image::RGB, width, height, false);
    juce::Random random (1990);

    // Two 1-D noise tables from the same white noise, box-smoothed: long streaks (radius 48)
    // and fine grain (radius 1), each normalised to a peak of 1.
    constexpr int kTable = 8192;
    std::vector<float> white (kTable), streaks (kTable), grain (kTable);
    for (auto& v : white)
        v = random.nextFloat() * 2.0f - 1.0f;
    const auto smooth = [&white] (std::vector<float>& out, int radius)
    {
        // Running sum over a circular window.
        float sum = 0.0f;
        for (int k = -radius; k <= radius; ++k)
            sum += white[static_cast<size_t> ((k + kTable) % kTable)];
        float peak = 0.0f;
        for (int i = 0; i < kTable; ++i)
        {
            out[static_cast<size_t> (i)] = sum;
            peak = std::max (peak, std::abs (sum));
            sum += white[static_cast<size_t> ((i + radius + 1) % kTable)] - white[static_cast<size_t> ((i - radius + kTable) % kTable)];
        }
        for (auto& v : out)
            v /= peak;
    };
    smooth (streaks, 48);
    smooth (grain, 1);

    // The highlights are separable Gaussians: exp(-(dx^2 + dy^2)) = exp(-dx^2) exp(-dy^2).
    const float w = static_cast<float> (width), h = static_cast<float> (height);
    const auto gauss = [] (float d) { return std::exp (-d * d); };
    std::vector<float> lightX1 (static_cast<size_t> (width)), lightX2 (static_cast<size_t> (width));
    for (int x = 0; x < width; ++x)
    {
        const float fx = static_cast<float> (x);
        lightX1[static_cast<size_t> (x)] = 0.55f * gauss ((fx - 0.14f * w) / (0.20f * w));
        lightX2[static_cast<size_t> (x)] = 0.47f * gauss ((fx - 0.80f * w) / (0.22f * w));
    }

    juce::Image::BitmapData pixels (image, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < height; ++y)
    {
        const float row = random.nextFloat() * 2.0f - 1.0f;
        const int streakOffset = random.nextInt (kTable);
        const int grainOffset = random.nextInt (kTable);
        const float fy = static_cast<float> (y);
        const float lightY1 = gauss ((fy - 0.30f * h) / (0.42f * h));
        const float lightY2 = gauss ((fy - 0.48f * h) / (0.46f * h));
        for (int x = 0; x < width; ++x)
        {
            const float base = 28.0f + 4.0f * row + 8.0f * streaks[static_cast<size_t> ((x + streakOffset) % kTable)]
                             + 3.5f * grain[static_cast<size_t> ((x + grainOffset) % kTable)];
            const float light = 1.0f + lightX1[static_cast<size_t> (x)] * lightY1 + lightX2[static_cast<size_t> (x)] * lightY2;
            const float v = std::min (base * light, 67.0f);   // 0x43
            const auto grey = static_cast<juce::uint8> (std::max (0.0f, v));
            const auto blue = static_cast<juce::uint8> (std::max (0.0f, std::min (v + 3.0f, 70.0f)));   // 0x46
            pixels.setPixelColour (x, y, juce::Colour (grey, grey, blue));
        }
    }
    return image;
}

} // namespace rcv::theme
