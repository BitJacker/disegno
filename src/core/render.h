// Software rasteriser used for the on-screen preview and for tests.
#pragma once

#include <cstddef>
#include <vector>

#include "image.h"
#include "strokes.h"

namespace dz {

// Draws strokes[first, last) onto `canvas` (1 = paper, 0 = ink). Area coordinates are
// mapped with out = offset + p * scale. `lineWidth` is in output pixels.
void renderStrokes(Gray& canvas, const std::vector<Stroke>& strokes, float scale, float offX, float offY,
                   float lineWidth, size_t first = 0, size_t last = static_cast<size_t>(-1));

}  // namespace dz
