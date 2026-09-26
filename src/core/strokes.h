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
};

// Mouse timing shared by the drawing engine and the time estimate.
struct Timing {
    float stepPx = 6;        // max distance between mouse positions while pressed (0 = vertices only)
    float moveDelayMs = 2;   // pause after each move while pressed
    float downDelayMs = 8;   // pause after reaching a stroke start and after pressing
    float upDelayMs = 8;     // pause before and after releasing
    bool jiggle = false;     // 1px wiggle before pressing (lets games notice the hover)
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

double estimateSeconds(const std::vector<Stroke>& strokes, const Timing& t);

}  // namespace dz
