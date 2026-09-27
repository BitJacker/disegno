// Moves the real mouse to draw strokes in any application (SendInput).
#pragma once

#include <vector>

#include "../core/strokes.h"
#include "winutil.h"

enum class DrawResult : int {
    Completed = 0,
    Stopped = 1,    // ESC or the stop button
    UserMoved = 2,  // the user grabbed the mouse (safety stop)
};

struct DrawJob {
    std::vector<dz::Stroke> strokes;  // in area coordinates
    POINT origin{};                   // screen position of the area's top-left corner
    dz::Timing timing;
    bool failsafe = true;             // stop when the user moves the mouse
    bool relative = false;            // relative moves, for games that ignore absolute input
    HWND syncWindow = nullptr;        // window under the area: waited for when the timing syncs
    HWND notify = nullptr;            // receives WM_APP_DRAW_PROGRESS / _PAUSED / _DONE
};

namespace mouse {

// Starts drawing on a background thread; takes ownership of `job`.
bool start(DrawJob* job);
bool running();
void requestStop();

}  // namespace mouse
