#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Every visual constant of the editor lives here (docs/PLUGIN_SPECS.md "Editor"): colours,
// font sizes, spacing, control and region sizes, strokes, radii and UI timings. Nothing
// else in plugin/src/ui hard-codes one; the other files only combine these (sums, halves)
// and use layout proportions and counts.
//
// Style rules: neutral greys plus one accent, flat fills, 1 px borders, 4 px corner radius,
// no gradients, shadows, glow or textures, one sans-serif family, hierarchy by size and
// weight only. Motion is limited to control-state transitions of kTransitionMs.
//
// Grid: every spacing, height and width is a multiple of 4 px. The only exceptions are
// the 1 px borders and hairlines, the strokes below and kOpticalOffset (2 px), used to
// centre a text line or a knob optically inside a 4-px-grid box.

namespace rcv::theme
{

// ----- editor geometry ----------------------------------------------------------------------
// The editor is laid out once at 1280 x 720 logical px and scaled as a whole by ui_scale.
inline constexpr int kBaseWidth = 1280;
inline constexpr int kBaseHeight = 720;
inline constexpr float kMinScale = 1.0f;
inline constexpr float kMaxScale = 2.0f;

// Scales offered by the UI scale box, in percent. Steps of 50 % keep 1 px lines on whole or
// half device pixels; 125 % and 175 % blurred every hairline and are not offered (the
// corner can still resize freely).
inline constexpr int kScaleChoicesPercent[] = { 100, 150, 200 };

// Regions of the 1280 x 720 layout (ui/EditorLayout.h).
inline constexpr int kHeaderHeight = 40;
inline constexpr int kSidebarWidth = 208;
inline constexpr int kScopeHeight = 56;
inline constexpr int kHostWindowAllowance = 64;  // screen height kept free for the host window frame

// ----- colours --------------------------------------------------------------------------------
// WCAG 2.x contrast ratios (relative luminance, sRGB) measured for the pairs that occur:
//   text      on background 14.2:1, on panel 12.9:1, on surface 11.0:1, on hover 9.4:1  (AA text >= 4.5)
//   textDim   on background  7.3:1, on panel  6.6:1, on surface  5.6:1                  (AA text >= 4.5)
//   accent    on background  6.3:1, on panel  5.7:1, on surface  4.9:1                  (AA text >= 4.5, graphics >= 3)
//   onAccent  on accent      6.8:1                                                      (text on filled toggles)
//   border    on panel       3.2:1, on background 3.5:1                                 (AA non-text >= 3)
//   accent    on track       3.3:1                                                      (knob value arc vs. its track)
//   textDisabled on panel    3.3:1 (disabled controls are exempt from the text minimum)
// The knob track (1.7:1 on panel) is decorative context only; the value arc and the
// pointer carry the information and both meet 3:1.
namespace colours
{
    inline const juce::Colour background   { 0xff18191c };  // editor background
    inline const juce::Colour panel        { 0xff212226 };  // group boxes, strips
    inline const juce::Colour surface      { 0xff2c2e33 };  // control bodies (buttons, combos, fields)
    inline const juce::Colour surfaceHover { 0xff363940 };  // hovered control body
    inline const juce::Colour border       { 0xff6b6f78 };  // 1 px outlines of controls
    inline const juce::Colour divider      { 0xff34363b };  // group outlines and separators (structure, not controls)
    inline const juce::Colour track        { 0xff44474e };  // knob arc background, scope grid line
    inline const juce::Colour text         { 0xffe6e7ea };  // primary text
    inline const juce::Colour textDim      { 0xffa3a7af };  // secondary text, captions, values
    inline const juce::Colour textDisabled { 0xff6e727a };
    inline const juce::Colour accent       { 0xff5b9fe3 };  // the only accent: value arcs, active toggles, focus, MIDI learn
    inline const juce::Colour onAccent     { 0xff0e1116 };  // text drawn on an accent fill
} // namespace colours

// ----- typography -----------------------------------------------------------------------------
// Sizes are em sizes in logical px (FontOptions::withPointHeight), like CSS font-size, so
// "13 px" really is 13 px at scale 1.0. Nothing is smaller than kFontBody.
inline constexpr float kFontBody = 13.0f;     // labels, values, menus, tooltips
inline constexpr float kFontGroup = 14.0f;    // group titles (bold)

// Windows' system sans-serif is Segoe UI; JUCE's generic sans-serif placeholder maps to
// Verdana on Windows, which is much wider and costs a column of knobs per group. Segoe UI
// is used when installed, otherwise the JUCE default sans-serif.
inline const juce::String& fontFamily()
{
    static const juce::String family = []
    {
        const auto names = juce::Font::findAllTypefaceNames();
        return names.contains ("Segoe UI") ? juce::String ("Segoe UI") : juce::Font::getDefaultSansSerifFontName();
    }();
    return family;
}

inline juce::Font font (float size = kFontBody, bool bold = false)
{
    return juce::Font (juce::FontOptions (fontFamily(), size, bold ? juce::Font::bold : juce::Font::plain)
                           .withPointHeight (size));
}

inline float textWidth (const juce::Font& f, const juce::String& text)
{
    return juce::GlyphArrangement::getStringWidth (f, text);
}

// ----- spacing (4 / 8 px grid) --------------------------------------------------------------
inline constexpr int kUnit = 4;
inline constexpr int kGap = 8;            // between controls and between groups
inline constexpr int kPad = 8;            // inner padding of group boxes and strips
inline constexpr float kBorder = 1.0f;    // every outline
inline constexpr float kRadius = 4.0f;    // every rounded corner
inline constexpr int kOpticalOffset = 2;  // optical centring of text lines and knobs (grid exception)

// ----- control sizes ----------------------------------------------------------------------------
// Density constraint: the Genesis panel shows 86 engine parameters and the NES panel 64 in
// the 1008 x 592 px panel area at scale 1.0, next to the header, the performance sidebar
// and the scopes. A knob cell (label + knob + value) is 64 px high and at least 56 px wide
// with a 28 px knob body; the operator grid uses 32 px rows with the value beside the
// knob. The whole cell is the drag target, so no interactive target is smaller than
// 24 x 24 px, and every control keeps a 13 px label. Denser layouts would need smaller type
// or hidden controls, which the design rules exclude.
inline constexpr int kLabelHeight = 16;          // one line of kFontBody
inline constexpr int kKnobDiameter = 28;         // every knob body
inline constexpr int kKnobMinWidth = 56;         // stacked knob cell
inline constexpr int kChoiceMinWidth = 40;       // combo box cell
// Stacked cell: label, 2 px, knob, 2 px, value = 16 + 2 + 28 + 2 + 16 = 64.
inline constexpr int kCellHeight = kLabelHeight + kOpticalOffset + kKnobDiameter + kOpticalOffset + kLabelHeight;
inline constexpr int kKnobCentreY = kLabelHeight + kOpticalOffset + kKnobDiameter / 2;   // knob line of a cell
inline constexpr int kControlHeight = 28;        // buttons, combo boxes, text fields, toggles
inline constexpr int kToggleStackGap = kGap;     // two toggles stacked in one cell: 28 + 8 + 28 = 64
inline constexpr int kClusterGap = 16;           // between clusters of related controls
inline constexpr int kClusterTitleHeight = 16;   // caption above a cluster of related controls
inline constexpr int kGroupTitleHeight = 24;     // group box header
inline constexpr int kGridRowHeight = 32;        // operator grid row (28 px control + 4)
inline constexpr int kGridCellMaxWidth = 64;     // operator grid column (wider combo values get an ellipsis)
inline constexpr int kMinTarget = 24;            // minimum interactive size at scale 1.0
inline constexpr int kComboArrowWidth = 20;      // arrow zone at the right of a combo box
inline constexpr int kPopupItemHeight = 24;      // popup menu, list and tooltip rows

// ----- header widgets -------------------------------------------------------------------------
inline constexpr int kSearchWidth = 160;         // preset search field
inline constexpr int kResultsWidth = 320;        // search results list under the field
inline constexpr int kResultsMaxRows = 12;       // rows shown before the list scrolls
inline constexpr int kScaleBoxWidth = 84;        // UI scale combo box

// ----- diagnostics overlay --------------------------------------------------------------------
inline constexpr int kDiagLabelColumn = 156;
inline constexpr int kDiagValueColumn = 72;

// ----- strokes and drawing details ------------------------------------------------------------
inline constexpr float kKnobArcThickness = 3.0f; // value arc and its track
inline constexpr float kKnobDiscInset = 2.0f;    // gap between the arc and the knob disc
inline constexpr float kKnobPointerThickness = 2.0f;
inline constexpr float kChevronHalfLength = 4.0f;   // menu / combo arrows: 8 x 4 px
inline constexpr float kChevronHalfDepth = 2.0f;
inline constexpr float kChevronThickness = 1.5f;
inline constexpr int kTickWidth = 4;             // accent bar of a ticked menu item
inline constexpr int kCaretWidth = 2;            // text caret (a stroke, not a spacing)
inline constexpr int kPopupBorder = kUnit;       // inner margin of popup menus
inline constexpr int kPopupSeparatorHeight = kGap;
inline constexpr int kPopupSeparatorWidth = 48;
inline constexpr int kPopupArrowSpace = 24;      // room for the submenu chevron
inline constexpr int kScrollbarWidth = 12;
inline constexpr int kScrollbarInset = 2;        // thumb inset inside the scrollbar (8 px thumb)
inline constexpr int kTooltipOffsetX = 16;       // tooltip distance from the mouse pointer
inline constexpr int kTooltipOffsetY = 20;
inline constexpr int kTooltipOffsetFlipped = 12; // when shown to the left of / above the pointer

// ----- motion and timing ----------------------------------------------------------------------
inline constexpr double kTransitionMs = 120.0;   // hover and toggle-state transitions
inline constexpr int kTooltipDelayMs = 700;
inline constexpr double kScopeIntervalMs = 16.0; // channel scopes read their rings at most at ~60 Hz

} // namespace rcv::theme
