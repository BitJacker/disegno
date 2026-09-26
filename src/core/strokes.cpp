#include "strokes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace dz {

float dist(const Pt& a, const Pt& b) { return std::hypot(a.x - b.x, a.y - b.y); }

double strokeLength(const Stroke& s) {
    double len = 0;
    for (size_t i = 1; i < s.pts.size(); ++i) len += dist(s.pts[i - 1], s.pts[i]);
    return len;
}

double totalLength(const std::vector<Stroke>& strokes) {
    double len = 0;
    for (const Stroke& s : strokes) len += strokeLength(s);
    return len;
}

size_t totalPoints(const std::vector<Stroke>& strokes) {
    size_t n = 0;
    for (const Stroke& s : strokes) n += s.pts.size();
    return n;
}

namespace {

float segDist2(const Pt& p, const Pt& a, const Pt& b) {
    float vx = b.x - a.x, vy = b.y - a.y;
    float wx = p.x - a.x, wy = p.y - a.y;
    float l2 = vx * vx + vy * vy;
    float t = l2 > 0 ? std::clamp((wx * vx + wy * vy) / l2, 0.f, 1.f) : 0.f;
    float dx = wx - t * vx, dy = wy - t * vy;
    return dx * dx + dy * dy;
}

}  // namespace

void simplify(std::vector<Pt>& pts, float epsilon) {
    const size_t n = pts.size();
    if (n < 3 || epsilon <= 0) return;
    std::vector<uint8_t> keep(n, 0);
    keep[0] = keep[n - 1] = 1;
    std::vector<std::pair<size_t, size_t>> stack;
    stack.emplace_back(0, n - 1);
    const float eps2 = epsilon * epsilon;
    while (!stack.empty()) {
        auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        float maxd = -1;
        size_t idx = a;
        for (size_t i = a + 1; i < b; ++i) {
            float d = segDist2(pts[i], pts[a], pts[b]);
            if (d > maxd) {
                maxd = d;
                idx = i;
            }
        }
        if (maxd > eps2) {
            keep[idx] = 1;
            stack.emplace_back(a, idx);
            stack.emplace_back(idx, b);
        }
    }
    size_t w = 0;
    for (size_t i = 0; i < n; ++i)
        if (keep[i]) pts[w++] = pts[i];
    pts.resize(w);
}

namespace {

// Orders strokes[first, last) greedily and appends them to out.
void orderRange(std::vector<Stroke>& strokes, size_t first, size_t last, Pt& cur, std::vector<Stroke>& out) {
    const size_t n = last - first;
    if (n == 0) return;
    std::vector<uint8_t> used(n, 0);

    float minx = 0, miny = 0, g = 1;
    int gx = 1, gy = 1;
    // Entries encode (local stroke index << 1) | end.
    std::vector<std::vector<uint32_t>> cells;
    size_t builtFor = 0;

    // (Re)builds the grid over the strokes not placed yet, so lookups stay cheap
    // even when only a few far-apart strokes remain.
    auto build = [&](size_t remaining) {
        minx = miny = std::numeric_limits<float>::max();
        float maxx = -minx, maxy = -minx;
        for (size_t i = 0; i < n; ++i) {
            if (used[i]) continue;
            const auto& p = strokes[first + i].pts;
            for (const Pt* q : {&p.front(), &p.back()}) {
                minx = std::min(minx, q->x);
                miny = std::min(miny, q->y);
                maxx = std::max(maxx, q->x);
                maxy = std::max(maxy, q->y);
            }
        }
        float bw = std::max(1.f, maxx - minx), bh = std::max(1.f, maxy - miny);
        g = std::max(4.f, std::sqrt(bw * bh / float(remaining)));
        gx = std::clamp(int(bw / g) + 1, 1, 2048);
        gy = std::clamp(int(bh / g) + 1, 1, 2048);
        g = std::max({g, bw / float(gx), bh / float(gy)}) + 1e-3f;
        cells.assign(size_t(gx) * size_t(gy), {});
        for (size_t i = 0; i < n; ++i) {
            if (used[i]) continue;
            const auto& p = strokes[first + i].pts;
            int fx = std::clamp(int((p.front().x - minx) / g), 0, gx - 1);
            int fy = std::clamp(int((p.front().y - miny) / g), 0, gy - 1);
            cells[size_t(fy) * gx + fx].push_back(uint32_t(i << 1));
            if (p.size() > 1) {
                int bx = std::clamp(int((p.back().x - minx) / g), 0, gx - 1);
                int by = std::clamp(int((p.back().y - miny) / g), 0, gy - 1);
                cells[size_t(by) * gx + bx].push_back(uint32_t((i << 1) | 1));
            }
        }
        builtFor = remaining;
    };
    build(n);

    auto cellX = [&](float x) { return std::clamp(int((x - minx) / g), 0, gx - 1); };
    auto cellY = [&](float y) { return std::clamp(int((y - miny) / g), 0, gy - 1); };

    for (size_t placed = 0; placed < n; ++placed) {
        const size_t remaining = n - placed;
        if (remaining >= 64 && remaining * 4 < builtFor) build(remaining);
        const int cx = cellX(cur.x), cy = cellY(cur.y);
        float best = std::numeric_limits<float>::max();
        int64_t bestEntry = -1;
        const int maxR = std::max(gx, gy);
        for (int r = 0; r <= maxR; ++r) {
            for (int yy = cy - r; yy <= cy + r; ++yy) {
                if (yy < 0 || yy >= gy) continue;
                const bool edgeRow = (yy == cy - r || yy == cy + r);
                for (int xx = cx - r; xx <= cx + r; xx += (edgeRow || r == 0) ? 1 : 2 * r) {
                    if (xx < 0 || xx >= gx) continue;
                    auto& cell = cells[size_t(yy) * gx + xx];
                    for (size_t k = 0; k < cell.size();) {
                        uint32_t e = cell[k];
                        size_t si = e >> 1;
                        if (used[si]) {
                            cell[k] = cell.back();
                            cell.pop_back();
                            continue;
                        }
                        const auto& p = strokes[first + si].pts;
                        const Pt& q = (e & 1) ? p.back() : p.front();
                        float d = (q.x - cur.x) * (q.x - cur.x) + (q.y - cur.y) * (q.y - cur.y);
                        if (d < best) {
                            best = d;
                            bestEntry = e;
                        }
                        ++k;
                    }
                }
            }
            if (bestEntry >= 0 && std::sqrt(best) <= float(r) * g) break;
        }
        if (bestEntry < 0) break;  // should not happen
        size_t si = size_t(bestEntry) >> 1;
        used[si] = 1;
        Stroke& s = strokes[first + si];
        if (bestEntry & 1) std::reverse(s.pts.begin(), s.pts.end());
        cur = s.pts.back();
        out.push_back(std::move(s));
    }
}

}  // namespace

void orderStrokes(std::vector<Stroke>& strokes, Pt start) {
    strokes.erase(std::remove_if(strokes.begin(), strokes.end(), [](const Stroke& s) { return s.pts.empty(); }),
                  strokes.end());
    if (strokes.size() < 2) return;
    std::stable_sort(strokes.begin(), strokes.end(),
                     [](const Stroke& a, const Stroke& b) { return a.layer < b.layer; });
    std::vector<Stroke> out;
    out.reserve(strokes.size());
    Pt cur = start;
    size_t i = 0;
    while (i < strokes.size()) {
        size_t j = i;
        while (j < strokes.size() && strokes[j].layer == strokes[i].layer) ++j;
        orderRange(strokes, i, j, cur, out);
        i = j;
    }
    strokes.swap(out);
}

void joinStrokes(std::vector<Stroke>& strokes, float maxGap) {
    if (strokes.size() < 2) return;
    std::vector<Stroke> out;
    out.reserve(strokes.size());
    out.push_back(std::move(strokes[0]));
    for (size_t i = 1; i < strokes.size(); ++i) {
        Stroke& prev = out.back();
        Stroke& next = strokes[i];
        if (next.pts.empty()) continue;
        float gap = dist(prev.pts.back(), next.pts.front());
        if (next.layer == prev.layer && gap <= maxGap) {
            size_t skip = gap < 0.01f ? 1 : 0;
            prev.pts.insert(prev.pts.end(), next.pts.begin() + std::ptrdiff_t(skip), next.pts.end());
        } else {
            out.push_back(std::move(next));
        }
    }
    strokes.swap(out);
}

int movesForSegment(float len, float stepPx) {
    if (stepPx <= 0) return 1;
    return std::max(1, int(std::ceil(len / stepPx)));
}

double estimateSeconds(const std::vector<Stroke>& strokes, const Timing& t) {
    const double evt = 0.05;  // rough cost of one injected event, ms
    double ms = 0;
    for (const Stroke& s : strokes) {
        if (s.pts.empty()) continue;
        ms += evt + t.downDelayMs;                           // travel + settle
        if (t.jiggle) ms += 2 * (evt + t.moveDelayMs);       // wiggle
        ms += evt + t.downDelayMs;                           // press
        if (s.pts.size() == 1) ms += 2 * (evt + t.moveDelayMs);  // dot nudge
        for (size_t i = 1; i < s.pts.size(); ++i)
            ms += movesForSegment(dist(s.pts[i - 1], s.pts[i]), t.stepPx) * (evt + t.moveDelayMs);
        ms += t.upDelayMs + evt + t.upDelayMs;               // release
    }
    return ms / 1000.0;
}

}  // namespace dz
