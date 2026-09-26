// Turns a picture into mouse strokes.
#pragma once

#include <atomic>

#include "image.h"
#include "strokes.h"

namespace dz {

enum class Style : int {
    Outline = 0,  // contours only (fastest)
    Sketch = 1,   // contours + cross-hatched shading (most "drawn" look)
    Hatch = 2,    // shading only, pen-and-ink style
    Dots = 3,     // halftone rows, closest to the photo (slowest)
};
constexpr int kStyleCount = 4;

struct Params {
    Style style = Style::Sketch;
    int detail = 6;        // 1..10
    int shading = 5;       // 0..10 (darkness of shading)
    float brush = 2.f;     // pen thickness in the target app, pixels
    bool invert = false;   // treat light areas as ink (white pen on dark paper)
    bool stretch = false;  // fill the area, ignoring the aspect ratio
};

// Where the picture lands inside the target area.
struct Layout {
    float ox = 0, oy = 0, w = 0, h = 0;
};
Layout fitLayout(int imgW, int imgH, float areaW, float areaH, bool stretch);

// Builds strokes (in area coordinates) for an area of areaW x areaH pixels.
// Returns false when cancelled through `cancel`.
bool buildDrawing(const Gray& img, float areaW, float areaH, const Params& p, Drawing& out,
                  const std::atomic<bool>* cancel = nullptr);

}  // namespace dz
