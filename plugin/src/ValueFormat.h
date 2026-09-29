#pragma once

#include <juce_core/juce_core.h>

#include "Parameters.h"

namespace rcv
{

// Display text of parameter values, shared by the editor (knob values) and the host
// (APVTS stringFromValue / valueFromString, so automation lanes and generic editors show the
// same text as the panels). The engine value and range stay the register value; only the
// text is converted.

// A native engine value as text: choice label, "On"/"Off", or number + unit, with hardware
// units that read better converted: SN76489 attenuation ("x2 dB") and YM2612 total level
// ("TL") in dB, the SNES echo delay (EDL) in ms, register steps as "steps".
juce::String formatNativeValue (const ParamInfo& info, float native);

// Inverse of formatNativeValue(): accepts every text it produces, plus plain numbers in the
// displayed unit ("-6" for "-6 dB", "48" for "48 ms"). The result is clamped to the
// parameter range (and rounded for integer parameters); text without a number, or an
// unknown label, gives the parameter's default value.
float parseNativeValue (const ParamInfo& info, const juce::String& text);

} // namespace rcv
