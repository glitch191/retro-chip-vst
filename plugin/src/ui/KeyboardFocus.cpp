#include "ui/KeyboardFocus.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace rcv
{

void releaseKeyboardFocus (juce::Component& editorComponent)
{
    juce::Component::unfocusAllComponents();

   #if JUCE_WINDOWS
    // A click on a JUCE component that takes focus calls SetFocus() on the editor's window;
    // without handing the native focus back, keys would keep going to the editor's window
    // (which forwards unused ones to its parent with PostMessage) instead of the host.
    if (auto* peer = editorComponent.getPeer())
    {
        auto* hwnd = static_cast<HWND> (peer->getNativeHandle());
        if (hwnd == nullptr || (GetWindowLongPtr (hwnd, GWL_STYLE) & WS_CHILD) == 0)
            return;   // top-level window (standalone): nothing to give back
        auto* focused = GetFocus();
        if (auto* parent = GetParent (hwnd); parent != nullptr && (focused == hwnd || IsChild (hwnd, focused)))
            SetFocus (parent);
    }
   #else
    juce::ignoreUnused (editorComponent);
   #endif
}

} // namespace rcv
