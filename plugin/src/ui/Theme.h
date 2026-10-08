#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Every visual constant of the editor lives here (docs/PLUGIN_SPECS.md "Editor"): colours,
// font sizes, spacing, control and region sizes, strokes, radii and UI timings. Nothing
// else in plugin/src/ui hard-codes one; the other files only combine these (sums, halves)
// and use layout proportions and counts. The drawing helpers of the faceplate style are in
// ui/Theme.cpp.
//
// Style (owner decision, 2026-10-08): the front panel of a 1990s rack module. A dark
// brushed-metal faceplate generated in code; sections without boxes, titled in light blue
// silkscreen capitals over a thin line with a short bracket tick at each end; secondary
// print (control names, captions) in orange silkscreen; dark rectangular buttons with a
// slight vertical gradient, a black edge and a light top line, selection buttons carrying
// a small red LED in the top-left corner; recessed black windows for lists, text fields,
// displays and the diagnostics; list and menu selection in blue with amber text. Lettering:
// Bahnschrift. Motion is limited to control-state transitions of kTransitionMs.
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
// WCAG 2.x contrast (relative luminance, sRGB) of every text colour against the brightest
// point of the faceplate (#434346, the limit makeFaceplate() never exceeds), minimum 4.5:1,
// then against the plain faceplate (#222225) and a recessed window (#0C0D0F):
//   text        #E8E8E6  8.0:1  12.9:1  15.8:1
//   textDim     #AEB2B8  4.6:1   7.4:1   9.1:1
//   silk        #86BCEB  4.9:1   7.9:1   9.6:1
//   silkOrange  #E8A24C  4.6:1   7.3:1   9.0:1
//   listAmber   #DCC870  5.9:1   9.5:1  11.6:1   (MIDI learn value; list selection text)
// On their own backgrounds: text on a button 8.8:1 (top, #3D3D42) to 10.4:1 (bottom,
// #323236); textDim on a button 5.1:1 to 6.0:1; listAmber on listBlue 5.4:1.
// Decorative colours never used for text: the red LED (#FF4636, 2.9:1 on #434346) and the
// selection blue. textDisabled (2.0:1) marks disabled controls, which are exempt from the
// text minimum. The knob value arc and the scope traces use silk (graphics, >= 3:1).
namespace colours
{
    inline const juce::Colour faceplate       { 0xff222225 };  // reference colour of the brushed metal, floating windows
    inline const juce::Colour faceplateBright { 0xff434346 };  // brightest point of the faceplate (contrast reference)
    inline const juce::Colour recess          { 0xff0c0d0f };  // recessed black windows
    inline const juce::Colour recessHover     { 0xff1b1d22 };
    inline const juce::Colour control         { 0xff323236 };  // button body, bottom of its gradient
    inline const juce::Colour controlTop      { 0xff3d3d42 };  // button body, top of its gradient
    inline const juce::Colour controlEdge     { 0xff0a0a0b };  // edges of buttons and windows
    inline const juce::Colour separator       { 0xff38383d };  // menu separators, scroll bar thumb
    inline const juce::Colour rule            { 0x73000000 };  // thin dark lines engraved in the faceplate
    inline const juce::Colour text            { 0xffe8e8e6 };  // primary text, values on buttons
    inline const juce::Colour textDim         { 0xffaeb2b8 };  // values, secondary text
    inline const juce::Colour textDisabled    { 0xff6e727a };
    inline const juce::Colour silk            { 0xff86bceb };  // light blue silkscreen: section titles, knob arcs, scope traces
    inline const juce::Colour silkOrange      { 0xffe8a24c };  // orange silkscreen: control names, captions
    inline const juce::Colour tick            { 0xff6f7378 };  // resizer, scale marks
    inline const juce::Colour listBlue        { 0xff2234c0 };  // selection in a list or a menu
    inline const juce::Colour listAmber       { 0xffdcc870 };  // text of a selection; MIDI learn
    inline const juce::Colour ledOn           { 0xffff4636 };
    inline const juce::Colour ledOff          { 0xff431816 };
} // namespace colours

// ----- typography -----------------------------------------------------------------------------
// Sizes are em sizes in logical px (FontOptions::withPointHeight), like CSS font-size, so
// "13 px" really is 13 px at scale 1.0. Nothing is smaller than kFontBody.
inline constexpr float kFontBody = 13.0f;     // labels, values, menus, tooltips, silkscreen names
inline constexpr float kFontGroup = 14.0f;    // section titles (silkscreen)
inline constexpr float kFontProduct = 24.0f;  // product name in the header (italic)
inline constexpr float kSilkSpacing = 0.05f;  // extra kerning of the silkscreen capitals
inline constexpr float kMinHorizontalScale = 0.6f;   // tightest squeeze of a knob name before it is cut
inline constexpr float kNameLayoutScale = 0.8f;      // share of a name's width a knob cell reserves (the rest is squeezed)

// Lettering: Bahnschrift, a DIN 1451 design shipped with Windows 10 and 11, in its Regular
// and SemiBold cuts for text and SemiBold SemiCondensed, slightly spaced, for the
// silkscreen. Without it, JUCE's default sans-serif.
//
// Optional user typeface: the first .ttf or .otf file (by name) in
// %APPDATA%\retro-chip-vst\fonts replaces Bahnschrift everywhere, drawn at
// kUserTypefaceScale of the size. Loaded at run time only, never bundled. Every open editor
// holds it (acquireUserTypeface() in its constructor, releaseUserTypeface() in its
// destructor) and the last one closed frees it: a static reference destroyed when the
// plugin is unloaded, after JUCE's font system, crashes the host. Message thread only.
//
// Layout never depends on the user typeface: widths are measured with the layout fonts
// (always Bahnschrift), and text drawn with a wider user typeface is squeezed into them
// (drawFittedText, down to kMinHorizontalScale) before it is cut with an ellipsis. So the
// panels keep every control in place whatever the typeface.
inline constexpr float kUserTypefaceScale = 0.9f;
const juce::String& fontFamily();
juce::Font font (float size = kFontBody, bool bold = false);
juce::Font silkFont (float size = kFontBody, float spacing = kSilkSpacing);
juce::Font layoutFont (float size = kFontBody, bool bold = false);
juce::Font layoutSilkFont (float size = kFontBody, float spacing = kSilkSpacing);
void acquireUserTypeface();
void releaseUserTypeface();
void setUserTypefaceAllowed (bool allowed);   // false: Bahnschrift even with a user typeface (screenshots)

inline float textWidth (const juce::Font& f, const juce::String& text)
{
    return juce::GlyphArrangement::getStringWidth (f, text);
}

// Control names (knobs, combo boxes, sidebar rows): orange silkscreen capitals without extra
// spacing, squeezed down to kMinHorizontalScale rather than cut.
inline juce::Font nameFont() { return silkFont (kFontBody, 0.0f); }
inline juce::Font layoutNameFont() { return layoutSilkFont (kFontBody, 0.0f); }

// Layout width of a silkscreen caption (drawn in capitals).
inline float silkWidth (const juce::String& text, float size = kFontBody, float spacing = kSilkSpacing)
{
    return textWidth (layoutSilkFont (size, spacing), text.toUpperCase());
}

// Drawn width of a silkscreen caption, at most `available` (the text is squeezed to fit).
inline float drawnSilkWidth (const juce::String& text, float available, float size = kFontBody, float spacing = kSilkSpacing)
{
    return juce::jmin (available, textWidth (silkFont (size, spacing), text.toUpperCase()));
}

// ----- spacing (4 / 8 px grid) --------------------------------------------------------------
inline constexpr int kUnit = 4;
inline constexpr int kGap = 8;            // between controls and between groups
inline constexpr int kPad = 8;            // inner padding of group boxes and strips
inline constexpr float kBorder = 1.0f;    // every outline
inline constexpr float kRadius = 2.0f;    // every rounded corner (square hardware buttons)
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
inline constexpr int kGroupTitleHeight = 24;     // section title and its bracket line
inline constexpr int kGridRowHeight = 32;        // operator grid row (28 px control + 4)
inline constexpr int kGridCellMaxWidth = 64;     // operator grid column (wider combo values get an ellipsis)
inline constexpr int kMinTarget = 24;            // minimum interactive size at scale 1.0
inline constexpr int kComboArrowWidth = 20;      // arrow zone at the right of a combo box
inline constexpr int kPopupItemHeight = 24;      // popup menu, list and tooltip rows

// ----- header widgets -------------------------------------------------------------------------
inline constexpr int kSearchWidth = 144;         // preset search field
inline constexpr int kResultsWidth = 640;        // search results list under the field (right-aligned to it)
inline constexpr int kResultsMaxRows = 16;       // rows shown before the list scrolls (24 px each)
inline constexpr int kChipTagWidth = 44;         // "NES" / "SNES" / "GEN" label of a result row
inline constexpr int kChipTagHeight = 20;        // inside the 24 px row: 2 px above and below
inline constexpr int kResultsCategoryWidth = 160;  // "Category / Subcategory" column of a result row
inline constexpr int kScaleBoxWidth = 72;        // UI scale combo box
inline constexpr int kModelLineWidth = 96;       // "CHIPTUNE / SOUND MODULE" beside the product name
inline constexpr int kHeaderGap = 8;             // between the header's groups (kClusterGap would not fit at 1280 px)

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
inline constexpr float kBracketTick = 6.0f;      // downward ticks at the ends of a section line
inline constexpr float kLedWidth = 10.0f;        // red LED of a selection button
inline constexpr float kLedHeight = 3.0f;
inline constexpr float kLedInsetX = 5.0f;        // from the button's top-left corner
inline constexpr float kLedInsetY = 4.0f;
inline constexpr float kLedGlow = 2.0f;          // halo around a lit LED
inline constexpr int kTickWidth = 4;             // amber bar of a ticked menu item
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

// ----- faceplate drawing (ui/Theme.cpp) -------------------------------------------------------
// A section of the faceplate: no box, a thin silkscreen line with a short downward tick at
// each end. `lineY` is the y of the line.
void drawBracket (juce::Graphics& g, float left, float right, float lineY);
// A section title in silkscreen capitals over its bracket line, at the top of `area`.
void drawSectionTitle (juce::Graphics& g, const juce::String& title, juce::Rectangle<int> area);
// A recessed black window (lists, text fields, displays, the diagnostics).
void drawRecess (juce::Graphics& g, juce::Rectangle<float> area);
// Dark rectangular button face; `hover` 0..1 lightens it.
void drawButtonFace (juce::Graphics& g, juce::Rectangle<float> area, bool enabled, float hover, bool down);
// Red LED in the top-left corner of a selection button; `on` 0..1 fades it.
void drawLed (juce::Graphics& g, juce::Rectangle<float> buttonArea, float on);
// The brushed metal faceplate, generated (no image file) at the given pixel size.
// Deterministic; callers cache it per size.
juce::Image makeFaceplate (int width, int height);

} // namespace rcv::theme
