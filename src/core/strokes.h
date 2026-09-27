// Stroke containers plus ordering, simplification and timing estimates.
#pragma once

#include <cstddef>
#include <vector>

namespace dz {

struct Pt {
    float x = 0;
    float y = 0;
};

// A polyline drawn with the mouse button held down. A single point is a dot/click.
// Layer 0 holds outlines; shading passes use higher layers and are drawn later.
struct Stroke {
    std::vector<Pt> pts;
    int layer = 0;
};

struct Drawing {
    float width = 0;   // size of the target area in pixels
    float height = 0;
    float brush = 1;   // pen thickness the strokes were built for
    std::vector<Stroke> strokes;
    // Filled by buildDrawingFor when a time limit applies.
    float coarse = 1;        // detail reduction that was needed (1 = none)
    size_t fullCount = 0;    // strokes before cutting
    bool trimmed = false;    // strokes had to be cut to fit the time
    float slow = 1;          // the timing may be this many times slower and still fit (spare time)
};

// Mouse timing shared by the drawing engine and the time estimate.
//
// With stepPx > 0 the mouse glides along each stroke in small steps, for apps that handle
// every mouse message (Paint & co.): downDelayMs is the pause before and after pressing,
// upDelayMs the pause before and after releasing.
// With stepPx <= 0 it jumps from vertex to vertex, for games that look at the mouse once
// per frame and join what they see with straight lines. moveDelayMs is then a frame the
// drawing must survive: the mouse rests that long on every vertex and at the start of each
// stroke before pressing. Many games also notice a press or a release a few frames late
// while they follow the mouse live; if the mouse left too soon they would miss the start
// of the stroke or draw the jump to the next one. So after pressing the mouse stays still
// for downDelayMs, and after releasing for upDelayMs.
struct Timing {
    float stepPx = 6;        // max distance between mouse positions while pressed (0 = frame mode)
    float moveDelayMs = 2;   // pause after each move while pressed
    float downDelayMs = 8;   // pause after pressing (and before, when stepPx > 0)
    float upDelayMs = 8;     // pause after releasing (and before, when stepPx > 0)
    bool jiggle = false;     // 1px wiggle before pressing (lets games notice the hover)
    bool sync = false;       // frame mode: after each press and release, also wait for the
                             // game window to take the input (a frozen game is waited for)
};

// The same timing with every pause `factor` times longer.
Timing slowed(const Timing& t, float factor);

// Expected wait for the game window when syncing: half a frame at 60 fps.
constexpr float kSyncWaitMs = 8.3f;

// Speed presets shared by the app and the tools.
inline constexpr Timing kTimingFast{10, 1, 4, 4, false};       // Paint, Photoshop, Krita...
inline constexpr Timing kTimingNormal{8, 4, 25, 25, false};
inline constexpr Timing kTimingWeb{12, 8, 25, 25, false};      // drawing websites
inline constexpr Timing kTimingGame{0, 34, 70, 70, false, true};       // games: Roblox & co.
inline constexpr Timing kTimingGameSlow{0, 50, 110, 110, false, true};  // slow games, or that stutter
inline constexpr Timing kTimingSlow{3, 30, 70, 70, true};

// One mouse state: the engine puts the cursor at `at`, sets the button to `down` (never both
// in the same step) and keeps it like that for `ms` milliseconds. With `sync` it first waits
// until the game window has taken the input.
struct Step {
    Pt at;
    bool down = false;
    float ms = 0;
    bool sync = false;
};

float dist(const Pt& a, const Pt& b);
double strokeLength(const Stroke& s);
double totalLength(const std::vector<Stroke>& strokes);
size_t totalPoints(const std::vector<Stroke>& strokes);

// Ramer-Douglas-Peucker simplification (keeps both ends).
void simplify(std::vector<Pt>& pts, float epsilon);

// Greedy nearest-neighbour ordering, one layer after the other (ascending).
// Strokes may be reversed so the pen travels as little as possible.
void orderStrokes(std::vector<Stroke>& strokes, Pt start);

// Merges consecutive strokes of the same layer whose gap is at most maxGap.
void joinStrokes(std::vector<Stroke>& strokes, float maxGap);

// Number of mouse moves the engine sends for a pressed segment of length len.
int movesForSegment(float len, float stepPx);

// Adapts strokes to a timing: when the mouse moves once per vertex (stepPx <= 0, e.g.
// games that read the mouse once per frame) every vertex costs a frame, so polylines
// are simplified a little more.
void prepareForTiming(std::vector<Stroke>& strokes, const Timing& t);

// Appends the mouse steps that draw one stroke: go to the start with the button up, press,
// follow the stroke, release. The drawing engine, the time estimate and the game simulator
// all use this, so they always agree.
void planStroke(const Stroke& s, const Timing& t, std::vector<Step>& out);

// Time needed to draw one stroke / all strokes with the given timing, in seconds.
double strokeSeconds(const Stroke& s, const Timing& t);
double estimateSeconds(const std::vector<Stroke>& strokes, const Timing& t);

// Keeps the most important strokes so that drawing takes at most `seconds`: outlines before
// shading, and long lines before short ones. The drawing order of the kept strokes is kept.
std::vector<Stroke> fitToTime(const std::vector<Stroke>& strokes, const Timing& t, double seconds);

}  // namespace dz
