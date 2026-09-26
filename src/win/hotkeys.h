// System-wide ESC / F8 detection while picking an area or drawing.
// A low-level keyboard hook catches even very short key taps (polling can miss them) and
// swallows the keys, so ESC does not also reach the app being drawn on (e.g. game menus).
#pragma once

namespace hotkeys {

void enable();
void disable();
bool active();
bool takeStop();   // true once for every ESC press since the last call
bool takePause();  // true once for every F8 / Pause press

}  // namespace hotkeys
