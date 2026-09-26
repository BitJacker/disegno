#include "render.h"

#include <algorithm>
#include <cmath>

namespace dz {

namespace {

void capsule(Gray& g, Pt a, Pt b, float width) {
    const float r = std::max(0.f, width * 0.5f);
    const int x0 = std::max(0, int(std::floor(std::min(a.x, b.x) - r - 1)));
    const int y0 = std::max(0, int(std::floor(std::min(a.y, b.y) - r - 1)));
    const int x1 = std::min(g.w - 1, int(std::ceil(std::max(a.x, b.x) + r + 1)));
    const int y1 = std::min(g.h - 1, int(std::ceil(std::max(a.y, b.y) + r + 1)));
    if (x0 > x1 || y0 > y1) return;
    const float vx = b.x - a.x, vy = b.y - a.y;
    const float l2 = vx * vx + vy * vy;
    const bool thin = width < 1.f;
    for (int y = y0; y <= y1; ++y) {
        float py = float(y) + 0.5f;
        float* row = &g.p[size_t(y) * size_t(g.w)];
        for (int x = x0; x <= x1; ++x) {
            float px = float(x) + 0.5f;
            float t = l2 > 0 ? std::clamp(((px - a.x) * vx + (py - a.y) * vy) / l2, 0.f, 1.f) : 0.f;
            float dx = px - (a.x + t * vx), dy = py - (a.y + t * vy);
            float d = std::sqrt(dx * dx + dy * dy);
            // Thin lines spread their (reduced) ink over one pixel.
            float cov = thin ? width * std::clamp(1.f - d, 0.f, 1.f) : std::clamp(r + 0.5f - d, 0.f, 1.f);
            if (cov <= 0.f) continue;
            float v = 1.f - cov;
            if (v < row[x]) row[x] = v;
        }
    }
}

}  // namespace

void renderStrokes(Gray& canvas, const std::vector<Stroke>& strokes, float scale, float offX, float offY,
                   float lineWidth, size_t first, size_t last) {
    last = std::min(last, strokes.size());
    for (size_t i = first; i < last; ++i) {
        const auto& pts = strokes[i].pts;
        if (pts.empty()) continue;
        auto map = [&](const Pt& p) { return Pt{offX + p.x * scale, offY + p.y * scale}; };
        if (pts.size() == 1) {
            Pt q = map(pts[0]);
            capsule(canvas, q, q, lineWidth);
            continue;
        }
        Pt prev = map(pts[0]);
        for (size_t k = 1; k < pts.size(); ++k) {
            Pt q = map(pts[k]);
            capsule(canvas, prev, q, lineWidth);
            prev = q;
        }
    }
}

}  // namespace dz
