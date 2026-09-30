#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace rcv
{

// Gives the keyboard back to the host: no component keeps the JUCE keyboard focus, and
// when the editor sits in a host window on Windows (a child window) while it holds the
// native focus, the native focus returns to the host's parent window. The host's
// shortcuts and its computer-keyboard note input then receive the keys again. In the
// standalone app (a top-level window) only the JUCE focus is cleared.
void releaseKeyboardFocus (juce::Component& editorComponent);

} // namespace rcv
