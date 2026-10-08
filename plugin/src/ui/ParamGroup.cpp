#include "ui/ParamGroup.h"

#include "ui/Theme.h"

#include <algorithm>
#include <cmath>

namespace rcv
{

namespace
{
    juce::StringArray wordsOf (const juce::String& text)
    {
        juce::StringArray words;
        words.addTokens (text, " ", "");
        words.removeEmptyStrings();
        return words;
    }

    bool startsWithWords (const juce::StringArray& words, const juce::StringArray& prefix)
    {
        if (prefix.isEmpty() || words.size() < prefix.size())
            return false;
        for (int i = 0; i < prefix.size(); ++i)
            if (words[i] != prefix[i])
                return false;
        return true;
    }

    juce::StringArray slice (const juce::StringArray& words, int start, int end)
    {
        juce::StringArray out;
        for (int i = start; i < end; ++i)
            out.add (words[i]);
        return out;
    }

    juce::String join (const juce::StringArray& words)
    {
        return words.joinIntoString (" ");
    }
} // namespace

// ----- PanelSection -------------------------------------------------------------------------

void PanelSection::paintBox (juce::Graphics& g) const
{
    // No box: the title in silkscreen capitals over a bracket line across the section.
    theme::drawSectionTitle (g, title, getLocalBounds().removeFromTop (theme::kGroupTitleHeight));
}

juce::Rectangle<int> PanelSection::titleArea() const
{
    return getLocalBounds().removeFromTop (theme::kGroupTitleHeight - theme::kGap + theme::kOpticalOffset).withTrimmedLeft (theme::kOpticalOffset);
}

float PanelSection::titleWidth (const juce::String& text)
{
    return theme::silkWidth (text, theme::kFontGroup);
}

juce::String PanelSection::getTooltip()
{
    const auto position = getMouseXYRelative();
    const auto area = titleArea();
    if (area.contains (position))
        return titleWidth (title) > static_cast<float> (area.getWidth()) ? title : juce::String();
    return truncatedCaptionAt (position);
}

juce::String ParamGroup::truncatedCaptionAt (juce::Point<int> position) const
{
    for (const auto& cap : current.captions)
        if (cap.area.contains (position)
            && theme::textWidth (theme::nameFont(), cap.text.toUpperCase()) * theme::kMinHorizontalScale > static_cast<float> (cap.area.getWidth()))
            return cap.text;
    return {};
}

// ----- labels and clusters ------------------------------------------------------------------

std::vector<ParamGroup::Cluster> ParamGroup::clustersFor (const juce::String& groupName, const std::vector<const ParamInfo*>& params)
{
    // 1. Short names: drop the group prefix ("Pulse 1 Duty" in "Pulse 1" -> "Duty") unless
    //    what remains starts with a number ("FM 1 Pan" in "FM" stays whole). A parameter
    //    named exactly like its group is the group's on/off switch.
    const auto groupWords = wordsOf (groupName);
    std::vector<juce::StringArray> names;
    for (const auto* p : params)
    {
        auto words = wordsOf (p->desc.name != nullptr ? juce::String (p->desc.name) : p->engineKey());
        if (words == groupWords)
            words.clear();
        else if (startsWithWords (words, groupWords) && ! juce::CharacterFunctions::isDigit (words[groupWords.size()][0]))
            words = slice (words, groupWords.size(), words.size());
        names.push_back (words);
    }

    std::vector<Cluster> clusters;
    auto addSingle = [&clusters] (const ParamInfo* info, const juce::String& labelText)
    {
        if (clusters.empty() || clusters.back().title.isNotEmpty())
            clusters.push_back ({});
        clusters.back().items.push_back ({ info, labelText.isEmpty() ? juce::String ("Enable") : labelText });
    };

    // The software envelope (engine keys "<channel>_sw_attack" ...) is its own titled cluster,
    // so it never reads as part of the hardware envelope or of the cluster above it.
    auto isSoftwareEnvelope = [&params] (int index)
    {
        const auto key = params[static_cast<size_t> (index)]->engineKey();
        return key.contains ("_sw_") || key.startsWith ("sw_");
    };

    const int n = static_cast<int> (params.size());
    for (int i = 0; i < n;)
    {
        if (isSoftwareEnvelope (i))
        {
            int j = i + 1;
            while (j < n && isSoftwareEnvelope (j))
                ++j;
            if (j - i >= 2)
            {
                Cluster cluster;
                cluster.title = "Software envelope";
                for (int k = i; k < j; ++k)
                {
                    const auto labelText = join (names[static_cast<size_t> (k)]);
                    cluster.items.push_back ({ params[static_cast<size_t> (k)], labelText.isEmpty() ? juce::String ("Enable") : labelText });
                }
                clusters.push_back (std::move (cluster));
                i = j;
                continue;
            }
        }

        const auto& first = names[static_cast<size_t> (i)];
        int j = i + 1;
        if (! first.isEmpty())
            while (j < n && ! names[static_cast<size_t> (j)].isEmpty() && names[static_cast<size_t> (j)][0] == first[0])
                ++j;

        bool anyLonger = false;
        for (int k = i; k < j; ++k)
            anyLonger = anyLonger || names[static_cast<size_t> (k)].size() > 1;

        if (j - i < 2 || ! anyLonger)
        {
            addSingle (params[static_cast<size_t> (i)], join (first));
            ++i;
            continue;
        }

        // 2. A run sharing its leading word(s) becomes a titled cluster.
        int prefixLen = first.size();
        for (int k = i + 1; k < j; ++k)
        {
            const auto& w = names[static_cast<size_t> (k)];
            int common = 0;
            while (common < prefixLen && common < w.size() && w[common] == first[common])
                ++common;
            prefixLen = common;
        }

        std::vector<juce::StringArray> rest;
        for (int k = i; k < j; ++k)
        {
            const auto& w = names[static_cast<size_t> (k)];
            rest.push_back (slice (w, prefixLen, w.size()));
        }

        // 3. "1 Echo" .. "8 Echo": the shared last word joins the title, numbers stay as labels.
        juce::String suffix;
        bool sameSuffix = true;
        for (const auto& r : rest)
        {
            if (r.size() < 2)
            {
                sameSuffix = false;
                break;
            }
            if (suffix.isEmpty())
                suffix = r[r.size() - 1];
            sameSuffix = sameSuffix && r[r.size() - 1] == suffix;
        }

        Cluster cluster;
        cluster.title = join (slice (first, 0, prefixLen));
        if (sameSuffix && suffix.isNotEmpty())
            cluster.title << " " << suffix.toLowerCase();
        for (int k = i; k < j; ++k)
        {
            auto r = rest[static_cast<size_t> (k - i)];
            if (sameSuffix && suffix.isNotEmpty())
                r.remove (r.size() - 1);
            const auto labelText = join (r);
            cluster.items.push_back ({ params[static_cast<size_t> (k)], labelText.isEmpty() ? juce::String ("Enable") : labelText });
        }
        clusters.push_back (std::move (cluster));
        i = j;
    }
    return clusters;
}

// ----- ParamGroup ---------------------------------------------------------------------------

ParamGroup::ParamGroup (UiContext& ctx, juce::String titleIn, const std::vector<Cluster>& clusterList, ControlStyle style)
    : PanelSection (std::move (titleIn))
{
    for (const auto& cl : clusterList)
    {
        ClusterColumns cc;
        cc.title = cl.title;

        std::vector<ParamControl*> halfRun;
        auto flushHalves = [&cc, &halfRun]
        {
            const int count = static_cast<int> (halfRun.size());
            const int numColumns = (count + 1) / 2;
            for (int c = 0; c < numColumns; ++c)
            {
                Column col;
                col.half = true;
                col.top = halfRun[static_cast<size_t> (c)];
                col.width = halfRun[static_cast<size_t> (c)]->preferredWidth();
                if (c + numColumns < count)
                {
                    auto* bottom = halfRun[static_cast<size_t> (c + numColumns)];
                    col.bottom = bottom;
                    col.width = juce::jmax (col.width, bottom->preferredWidth());
                }
                cc.columns.push_back (col);
            }
            halfRun.clear();
        };

        // In a titled cluster the switches come first so they stack in one column
        // ("Sweep": Enable over Negate, then Period and Shift).
        auto items = cl.items;
        if (cl.title.isNotEmpty())
            std::stable_partition (items.begin(), items.end(),
                                   [] (const Item& it) { return it.info != nullptr && isToggleParam (*it.info); });

        for (const auto& item : items)
        {
            if (item.info == nullptr)
                continue;
            auto control = createParamControl (ctx, *item.info, item.label, style);
            addAndMakeVisible (*control);
            if (control->isHalfHeight())
            {
                halfRun.push_back (control.get());
            }
            else
            {
                flushHalves();
                Column col;
                col.top = control.get();
                col.width = control->preferredWidth();
                cc.columns.push_back (col);
            }
            controls.push_back (std::move (control));
        }
        flushHalves();
        if (! cc.columns.empty())
            clusters.push_back (std::move (cc));
    }
}

void ParamGroup::addExtraColumn (juce::Component& component, int width)
{
    addAndMakeVisible (component);
    ClusterColumns cc;
    Column col;
    col.top = &component;
    col.half = true;
    col.width = width;
    cc.columns.push_back (col);
    clusters.push_back (std::move (cc));
}

ParamControl* ParamGroup::controlFor (const juce::String& paramId) const
{
    for (const auto& c : controls)
        if (c->paramInfo() != nullptr && c->paramInfo()->id == paramId)
            return c.get();
    return nullptr;
}

ParamGroup::Layout ParamGroup::computeLayout (int width) const
{
    struct Placed { int row; int x; int w; };
    struct Cap { int row; int x0; int x1; juce::String text; };

    const int contentW = juce::jmax (1, width - 2 * theme::kPad);
    std::vector<Placed> placed;
    std::vector<Cap> caps;
    std::vector<bool> rowTitled { false };
    int row = 0;
    int x = 0;   // right edge of the last placed column in the current row (0 = empty row)

    auto newRow = [&]
    {
        ++row;
        x = 0;
        rowTitled.push_back (false);
    };

    // Cluster boundaries stay visible: a cluster that does not fit the rest of a row starts
    // a new one (a software envelope stays on one row whenever the group is wide enough),
    // and an untitled cluster that follows a titled one gets a caption rule without text, so
    // it never reads as more controls of the caption above or beside it.
    bool previousTitled = false;
    for (const auto& cl : clusters)
    {
        int clusterW = -theme::kGap;
        for (const auto& col : cl.columns)
            clusterW += col.width + theme::kGap;

        int cx = x == 0 ? 0 : x + theme::kClusterGap;
        if (x > 0 && cx + clusterW > contentW)
        {
            newRow();
            cx = 0;
        }

        const bool titled = cl.title.isNotEmpty() || previousTitled;
        previousTitled = cl.title.isNotEmpty();
        int capRow = row;
        int capX = cx;
        bool captioned = false;
        bool firstInRow = true;
        for (const auto& col : cl.columns)
        {
            if (! firstInRow && cx + col.width > contentW)
            {
                if (titled && ! captioned)
                {
                    caps.push_back ({ capRow, capX, cx - theme::kGap, cl.title });
                    rowTitled[static_cast<size_t> (capRow)] = true;
                    captioned = true;
                }
                newRow();
                cx = 0;
            }
            placed.push_back ({ row, cx, col.width });
            cx += col.width + theme::kGap;
            firstInRow = false;
        }
        if (titled && ! captioned)
        {
            caps.push_back ({ capRow, capX, cx - theme::kGap, cl.title });
            rowTitled[static_cast<size_t> (capRow)] = true;
        }
        x = cx - theme::kGap;
    }

    Layout layout;
    std::vector<int> rowY, rowTop;
    int y = theme::kGroupTitleHeight;
    for (size_t r = 0; r < rowTitled.size(); ++r)
    {
        rowY.push_back (y);
        const int top = y + (rowTitled[r] ? theme::kClusterTitleHeight : 0);
        rowTop.push_back (top);
        y = top + theme::kCellHeight + theme::kGap;
    }
    layout.height = placed.empty() ? theme::kGroupTitleHeight + theme::kPad : y - theme::kGap + theme::kPad;

    for (const auto& p : placed)
        layout.columnBounds.push_back ({ theme::kPad + p.x, rowTop[static_cast<size_t> (p.row)], p.w, theme::kCellHeight });
    for (const auto& cap : caps)
        layout.captions.push_back ({ cap.text, { theme::kPad + cap.x0, rowY[static_cast<size_t> (cap.row)],
                                                 cap.x1 - cap.x0, theme::kClusterTitleHeight } });
    return layout;
}

int ParamGroup::heightForWidth (int width) const
{
    return computeLayout (width).height;
}

int ParamGroup::preferredWidth() const
{
    int w = 0;
    for (const auto& cl : clusters)
    {
        if (w > 0)
            w += theme::kClusterGap;
        int clusterW = -theme::kGap;
        for (const auto& col : cl.columns)
            clusterW += col.width + theme::kGap;
        w += clusterW;
    }
    const int titleW = static_cast<int> (std::ceil (titleWidth (title)));
    return juce::jmax (w, titleW) + 2 * theme::kPad;
}

void ParamGroup::resized()
{
    current = computeLayout (getWidth());

    size_t index = 0;
    for (const auto& cl : clusters)
    {
        for (const auto& col : cl.columns)
        {
            const auto b = current.columnBounds[index++];
            if (! col.half)
            {
                col.top->setBounds (b);
                continue;
            }
            const int h = theme::kControlHeight;
            if (col.bottom != nullptr)
            {
                col.top->setBounds (b.getX(), b.getY(), b.getWidth(), h);
                col.bottom->setBounds (b.getX(), b.getY() + h + theme::kToggleStackGap, b.getWidth(), h);
            }
            else
            {
                // A lone toggle sits on the knob line, like combo boxes do.
                const int centreY = b.getY() + theme::kKnobCentreY;
                col.top->setBounds (b.getX(), centreY - h / 2, b.getWidth(), h);
            }
        }
    }
}

void ParamGroup::paint (juce::Graphics& g)
{
    paintBox (g);

    // Cluster captions: orange silkscreen capitals, squeezed like the control names, and a
    // fine rule to the cluster's end.
    g.setFont (theme::nameFont());
    for (const auto& cap : current.captions)
    {
        const auto area = static_cast<float> (cap.area.getWidth());
        g.setColour (theme::colours::silkOrange);
        g.drawFittedText (cap.text.toUpperCase(), cap.area, juce::Justification::centredLeft, 1, theme::kMinHorizontalScale);
        const int textRight = cap.text.isEmpty() ? cap.area.getX()   // rule-only caption
                                                 : cap.area.getX() + static_cast<int> (std::ceil (theme::drawnSilkWidth (cap.text, area, theme::kFontBody, 0.0f))) + theme::kGap;
        if (textRight < cap.area.getRight())
        {
            g.setColour (theme::colours::silkOrange.withAlpha (0.35f));
            g.fillRect (textRight, cap.area.getCentreY(), cap.area.getRight() - textRight, 1);
        }
    }
}

} // namespace rcv
