// Calibration for games: rows of short dashes drawn in the chosen area, each row with a
// longer pause after every press and release. Looking at the screen afterwards tells which
// rows came out clean (no dash joined to the next one, no dash missing its start), that is
// how long this game needs to notice a click.
#pragma once

#include <cstdint>
#include <vector>

#include "strokes.h"

namespace dz {

// Pauses after each press and release that are tried, shortest first (about 1.4x apart).
constexpr int kCalibLevels = 6;
constexpr float kCalibClickMs[kCalibLevels] = {25, 40, 60, 85, 120, 170};

struct CalibRow {
    float y = 0;                 // the row's line, in area coordinates
    float clickMs = 0;           // pause after each press and release in this row
    Pt lead;                     // where the mouse waits before the row (left of the dashes)
    std::vector<Stroke> dashes;  // left to right
};

struct CalibPlan {
    std::vector<CalibRow> rows;
    float band = 2;  // half-height of the band around a row where its ink is looked for
};

// Rows for an area of areaW x areaH pixels drawn with a pen `brush` pixels thick. Empty when
// the area is too small.
CalibPlan planCalibration(float areaW, float areaH, float brush);

// The game timing for a pause of `clickMs` after each press and release.
Timing calibTiming(float clickMs);

struct CalibRowResult {
    double coverage = 0;  // share of the worst dash that has ink
    double stray = 0;     // share of the worst gap between dashes that has ink
    bool clean = false;
};

struct CalibResult {
    std::vector<CalibRowResult> rows;
    bool sawInk = false;  // anything changed on screen at all
    int pick = -1;        // fastest row from which every slower row is clean, or -1
};

// `before` and `after` are the area captured before and after drawing, w*h pixels in any
// 32-bit colour layout (only differences between the two count).
CalibResult analyzeCalibration(const CalibPlan& plan, const uint32_t* before, const uint32_t* after, int w, int h);

}  // namespace dz
