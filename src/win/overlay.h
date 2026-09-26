// Full-screen, click-through overlays: corner picking with countdown, area flash,
// countdown before drawing, and the small status box shown while drawing.
#pragma once

#include <string>

#include "winutil.h"

namespace overlay {

bool registerClasses(HINSTANCE inst);

// Two-corner capture: `seconds` to reach the top-left corner, then `seconds` for the
// bottom-right one. Posts WM_APP_AREA_PICKED to `notify` (lParam = new RECT on success).
void pickArea(HWND notify, int seconds);

// Highlights `area` on screen for `ms` milliseconds.
void flashArea(const RECT& area, int ms);

// Countdown shown next to `area`; posts WM_APP_COUNTDOWN_DONE to `notify`.
void countdown(HWND notify, const RECT& area, int seconds);

void cancel();
bool busy();

// Status box while drawing (never takes focus, never receives clicks).
void showStatus(const RECT& avoid);
void setStatus(const std::wstring& title, const std::wstring& line, double fraction, bool paused);
void hideStatus(int afterMs = 0);

}  // namespace overlay
