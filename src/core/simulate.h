// Emulates a game that looks at the mouse once per frame (like Roblox) to check how a
// drawing survives there: the game joins with straight lines the positions it sees while
// the button is down, and misses whatever happens between two frames.
#pragma once

#include <cstdint>
#include <vector>

#include "image.h"
#include "strokes.h"

namespace dz {

struct FrameModel {
    double fps = 60;
    double jitter = 0;   // every frame lasts up to this fraction longer, at random
    double hitch = 0;    // chance that a frame is late by 1..hitchFrames whole frames
    int hitchFrames = 2;
    bool pumps = true;   // the game takes window messages once per frame, so a sync waits for it
    uint32_t seed = 1;
};

// What the game would draw for `strokes` played with timing `t`: one polyline per press it
// noticed.
std::vector<Stroke> simulateFrames(const std::vector<Stroke>& strokes, const Timing& t, const FrameModel& fm);

// Differences between two renderings (1 = paper, 0 = ink), as fractions of the ink in
// `want`: `missing` is ink of `want` with no ink of `got` within `tolPx`, `extra` the
// opposite (lines that should not be there).
struct InkDiff {
    double missing = 0;
    double extra = 0;
};
InkDiff compareInk(const Gray& want, const Gray& got, int tolPx);

// Straight lines the game drew across the paper where `want` has no ink, such as two strokes
// joined because a release went unseen: segments at least minLen px long, mostly off the ink.
int countStrayLines(const Gray& want, const std::vector<Stroke>& seen, float minLen = 8.f);

}  // namespace dz
