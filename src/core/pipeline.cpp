#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dz {

namespace {

constexpr float kPi = 3.14159265358979f;

float lerp(float a, float b, float t) { return a + (b - a) * t; }

bool cancelled(const std::atomic<bool>* c) { return c && c->load(std::memory_order_relaxed); }

// 4-neighbours first, then diagonals.
const int DX[8] = {1, 0, -1, 0, 1, -1, -1, 1};
const int DY[8] = {0, 1, 0, -1, 1, 1, -1, -1};

// ---------------------------------------------------------------------------
// Edge detection (Canny) and thinning
// ---------------------------------------------------------------------------

std::vector<uint8_t> canny(const Gray& src, float sigma, float highQ, float lowRatio, float minHigh) {
    const Gray b = gaussianBlur(src, sigma);
    const int w = b.w, h = b.h;
    std::vector<uint8_t> out(size_t(w) * size_t(h), 0);
    if (w < 3 || h < 3) return out;

    std::vector<float> mag(size_t(w) * size_t(h), 0.f);
    std::vector<uint8_t> dir(size_t(w) * size_t(h), 0);
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            float tl = b.at(x - 1, y - 1), tc = b.at(x, y - 1), tr = b.at(x + 1, y - 1);
            float ml = b.at(x - 1, y), mr = b.at(x + 1, y);
            float bl = b.at(x - 1, y + 1), bc = b.at(x, y + 1), br = b.at(x + 1, y + 1);
            float gx = ((tr + 2 * mr + br) - (tl + 2 * ml + bl)) * 0.125f;
            float gy = ((bl + 2 * bc + br) - (tl + 2 * tc + tr)) * 0.125f;
            size_t i = size_t(y) * w + x;
            mag[i] = std::sqrt(gx * gx + gy * gy);
            float ax = std::fabs(gx), ay = std::fabs(gy);
            if (ay <= ax * 0.41421356f) dir[i] = 0;        // gradient ~horizontal
            else if (ay >= ax * 2.41421356f) dir[i] = 2;   // gradient ~vertical
            else dir[i] = (gx * gy > 0) ? 1 : 3;           // diagonals
        }
    }

    std::vector<float> nms(size_t(w) * size_t(h), 0.f);
    std::vector<float> vals;
    vals.reserve(size_t(w) * size_t(h) / 8);
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            size_t i = size_t(y) * w + x;
            float m = mag[i];
            if (m <= 1e-5f) continue;
            float a, c;
            switch (dir[i]) {
                case 0: a = mag[i - 1]; c = mag[i + 1]; break;
                case 2: a = mag[i - w]; c = mag[i + w]; break;
                case 1: a = mag[i - w - 1]; c = mag[i + w + 1]; break;
                default: a = mag[i - w + 1]; c = mag[i + w - 1]; break;
            }
            if (m > a && m >= c) {
                nms[i] = m;
                vals.push_back(m);
            }
        }
    }
    if (vals.empty()) return out;
    const float high = std::max(minHigh, quantile(vals, highQ));
    const float low = high * lowRatio;

    std::vector<size_t> stack;
    for (size_t i = 0; i < nms.size(); ++i) {
        if (nms[i] < high || out[i]) continue;
        out[i] = 1;
        stack.push_back(i);
        while (!stack.empty()) {
            size_t j = stack.back();
            stack.pop_back();
            int x = int(j % size_t(w)), y = int(j / size_t(w));
            for (int k = 0; k < 8; ++k) {
                int nx = x + DX[k], ny = y + DY[k];
                if (nx < 1 || ny < 1 || nx >= w - 1 || ny >= h - 1) continue;
                size_t n = size_t(ny) * w + nx;
                if (!out[n] && nms[n] >= low) {
                    out[n] = 1;
                    stack.push_back(n);
                }
            }
        }
    }
    return out;
}

// Zhang-Suen thinning followed by removal of redundant staircase pixels, so every
// line is one pixel wide and 8-connected. Border pixels must be zero.
void thin(std::vector<uint8_t>& m, int w, int h) {
    std::vector<size_t> clear;
    for (int iter = 0; iter < 40; ++iter) {
        bool changed = false;
        for (int pass = 0; pass < 2; ++pass) {
            clear.clear();
            for (int y = 1; y < h - 1; ++y) {
                for (int x = 1; x < w - 1; ++x) {
                    size_t i = size_t(y) * w + x;
                    if (!m[i]) continue;
                    int p2 = m[i - w], p3 = m[i - w + 1], p4 = m[i + 1], p5 = m[i + w + 1];
                    int p6 = m[i + w], p7 = m[i + w - 1], p8 = m[i - 1], p9 = m[i - w - 1];
                    int B = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
                    if (B < 2 || B > 6) continue;
                    int A = (!p2 && p3) + (!p3 && p4) + (!p4 && p5) + (!p5 && p6) + (!p6 && p7) + (!p7 && p8) +
                            (!p8 && p9) + (!p9 && p2);
                    if (A != 1) continue;
                    if (pass == 0) {
                        if (p2 && p4 && p6) continue;
                        if (p4 && p6 && p8) continue;
                    } else {
                        if (p2 && p4 && p8) continue;
                        if (p2 && p6 && p8) continue;
                    }
                    clear.push_back(i);
                }
            }
            if (!clear.empty()) changed = true;
            for (size_t i : clear) m[i] = 0;
        }
        if (!changed) break;
    }

    // Ring positions around a pixel (clockwise from north) and their mutual 8-adjacency.
    static const int RX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    static const int RY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            size_t i = size_t(y) * w + x;
            if (!m[i]) continue;
            int present[8], n = 0;
            for (int k = 0; k < 8; ++k)
                if (m[size_t(y + RY[k]) * w + (x + RX[k])]) present[n++] = k;
            if (n < 2) continue;  // keep end points
            // Count 8-connected components among the present neighbours.
            int comp[8];
            for (int a = 0; a < n; ++a) comp[a] = a;
            auto find = [&](int a) {
                while (comp[a] != a) a = comp[a] = comp[comp[a]];
                return a;
            };
            for (int a = 0; a < n; ++a)
                for (int b2 = a + 1; b2 < n; ++b2) {
                    int ka = present[a], kb = present[b2];
                    if (std::abs(RX[ka] - RX[kb]) <= 1 && std::abs(RY[ka] - RY[kb]) <= 1) comp[find(a)] = find(b2);
                }
            int comps = 0;
            for (int a = 0; a < n; ++a)
                if (find(a) == a) ++comps;
            if (comps == 1 && n <= 3) m[i] = 0;  // removing it cannot disconnect anything
        }
    }
}

using Chain = std::vector<Pt>;

// Follows 1-pixel wide lines into polylines (pixel coordinates).
std::vector<Chain> traceChains(const std::vector<uint8_t>& m, int w, int h) {
    std::vector<uint8_t> vis(size_t(w) * size_t(h), 0);
    std::vector<Chain> chains;
    auto id = [w](int x, int y) { return size_t(y) * size_t(w) + size_t(x); };
    auto count = [&](int x, int y) {
        int c = 0;
        for (int k = 0; k < 8; ++k) c += m[id(x + DX[k], y + DY[k])];
        return c;
    };

    auto trace = [&](int sx, int sy) {
        Chain pts;
        // Start from an already traced neighbour so branches touch their junction.
        for (int k = 0; k < 8; ++k) {
            int nx = sx + DX[k], ny = sy + DY[k];
            if (m[id(nx, ny)] && vis[id(nx, ny)]) {
                pts.push_back({float(nx), float(ny)});
                break;
            }
        }
        int x = sx, y = sy;
        vis[id(x, y)] = 1;
        pts.push_back({float(x), float(y)});
        for (;;) {
            float dx = 0, dy = 0;
            if (pts.size() >= 2) {
                const Pt& a = pts[pts.size() - std::min<size_t>(pts.size(), 4)];
                dx = float(x) - a.x;
                dy = float(y) - a.y;
                float l = std::hypot(dx, dy);
                if (l > 0) {
                    dx /= l;
                    dy /= l;
                }
            }
            int best = -1;
            float bestScore = -1e9f;
            for (int k = 0; k < 8; ++k) {
                int nx = x + DX[k], ny = y + DY[k];
                size_t j = id(nx, ny);
                if (!m[j] || vis[j]) continue;
                float s = k < 4 ? 0.15f : 0.f;
                s += (float(DX[k]) * dx + float(DY[k]) * dy) / (k < 4 ? 1.f : 1.41421356f);
                if (s > bestScore) {
                    bestScore = s;
                    best = k;
                }
            }
            if (best < 0) break;
            x += DX[best];
            y += DY[best];
            vis[id(x, y)] = 1;
            pts.push_back({float(x), float(y)});
        }
        // Close loops / reach a junction at the tail.
        if (pts.size() >= 3) {
            for (int k = 0; k < 8; ++k) {
                int nx = x + DX[k], ny = y + DY[k];
                size_t j = id(nx, ny);
                if (!m[j] || !vis[j]) continue;
                bool recent = false;
                for (size_t q = pts.size() >= 5 ? pts.size() - 5 : 0; q < pts.size(); ++q)
                    if (int(pts[q].x) == nx && int(pts[q].y) == ny) recent = true;
                if (recent) continue;
                pts.push_back({float(nx), float(ny)});
                break;
            }
        }
        return pts;
    };

    for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x)
            if (m[id(x, y)] && !vis[id(x, y)] && count(x, y) == 1) chains.push_back(trace(x, y));
    for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x)
            if (m[id(x, y)] && !vis[id(x, y)]) chains.push_back(trace(x, y));
    return chains;
}

float chainLength(const Chain& c) {
    float l = 0;
    for (size_t i = 1; i < c.size(); ++i) l += dist(c[i - 1], c[i]);
    return l;
}

void smoothChain(Chain& c, int iterations) {
    if (c.size() < 3) return;
    Chain tmp(c.size());
    for (int it = 0; it < iterations; ++it) {
        tmp.front() = c.front();
        tmp.back() = c.back();
        for (size_t i = 1; i + 1 < c.size(); ++i) {
            tmp[i].x = (c[i - 1].x + 2 * c[i].x + c[i + 1].x) * 0.25f;
            tmp[i].y = (c[i - 1].y + 2 * c[i].y + c[i + 1].y) * 0.25f;
        }
        c.swap(tmp);
    }
}

// ---------------------------------------------------------------------------
// Style generators
// ---------------------------------------------------------------------------

void addOutlines(const Gray& img, const Layout& L, float brush, float det, std::vector<Stroke>& out) {
    // One processing pixel per ~pen width, but never upsample the photo: when the
    // area is bigger than the picture the vectors are scaled up instead.
    const float cell = std::max(1.f, brush * lerp(1.0f, 0.55f, det));
    double pw = L.w / cell, ph = L.h / cell;
    const double up = std::max(pw / img.w, ph / img.h);
    if (up > 1) {
        pw /= up;
        ph /= up;
    }
    const double maxPix = 2.5e6;
    if (pw * ph > maxPix) {
        double s = std::sqrt(maxPix / (pw * ph));
        pw *= s;
        ph *= s;
    }
    const int W = std::max(8, int(pw + 0.5)), H = std::max(8, int(ph + 0.5));
    Gray g = resize(img, W, H);
    stretchContrast(g);
    localContrast(g, 8, 2.0f, 0.35f);

    const float sizeF = std::clamp(std::sqrt(float(W) * float(H)) / 700.f, 0.6f, 1.6f);
    const float sigma = lerp(2.1f, 0.9f, det) * sizeF;
    const float highQ = lerp(0.93f, 0.74f, det);
    std::vector<uint8_t> edges = canny(g, sigma, highQ, 0.4f, 0.012f);
    thin(edges, W, H);
    std::vector<Chain> chains = traceChains(edges, W, H);

    const float minLen = lerp(18.f, 5.f, det) * sizeF;
    const float sx = L.w / float(W), sy = L.h / float(H);
    for (Chain& c : chains) {
        if (chainLength(c) < minLen) continue;
        smoothChain(c, 2);
        simplify(c, 0.55f);
        Stroke s;
        s.layer = 0;
        s.pts.reserve(c.size());
        for (const Pt& q : c) s.pts.push_back({L.ox + (q.x + 0.5f) * sx, L.oy + (q.y + 0.5f) * sy});
        out.push_back(std::move(s));
    }
}

// Straight hatch lines at `angleDeg` covering every place where tone < thr.
void hatchLines(const Gray& tone, const Layout& L, float S, float angleDeg, float phase, float thr, float brush,
                int layer, std::vector<Stroke>& out) {
    const float a = angleDeg * kPi / 180.f;
    const float dx = std::cos(a), dy = std::sin(a);
    const float nx = -dy, ny = dx;
    const float x0 = L.ox, y0 = L.oy, x1 = L.ox + L.w, y1 = L.oy + L.h;
    const float cs[4] = {x0 * nx + y0 * ny, x1 * nx + y0 * ny, x0 * nx + y1 * ny, x1 * nx + y1 * ny};
    const float cmin = *std::min_element(cs, cs + 4), cmax = *std::max_element(cs, cs + 4);
    const float cw = L.w / float(tone.w), ch = L.h / float(tone.h);
    const float step = std::max(0.5f, std::min(cw, ch) * 0.5f);
    const float hyst = 0.025f;
    const float minRun = std::max(brush * 1.5f, S * 0.9f);
    const float maxGap = S * 0.5f;
    const float inset = brush * 0.5f;

    std::vector<std::pair<float, float>> runs;
    for (float c = cmin + S * (0.5f + phase); c < cmax; c += S) {
        float tmin = -1e9f, tmax = 1e9f;
        const float px0 = c * nx, py0 = c * ny;
        auto clip = [&](float p0, float d, float lo, float hi) {
            if (std::fabs(d) < 1e-6f) return p0 >= lo && p0 <= hi;
            float t1 = (lo - p0) / d, t2 = (hi - p0) / d;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            return true;
        };
        if (!clip(px0, dx, x0, x1) || !clip(py0, dy, y0, y1) || tmax - tmin < 1.f) continue;

        runs.clear();
        bool on = false;
        float start = 0;
        for (float t = tmin; t <= tmax; t += step) {
            float x = px0 + t * dx, y = py0 + t * dy;
            float v = tone.sample((x - L.ox) / cw - 0.5f, (y - L.oy) / ch - 0.5f);
            if (!on && v < thr) {
                on = true;
                start = t;
            } else if (on && v > thr + hyst) {
                on = false;
                runs.emplace_back(start, t - step);
            }
        }
        if (on) runs.emplace_back(start, tmax);

        // Bridge tiny interruptions, then drop crumbs.
        size_t w = 0;
        for (size_t i = 0; i < runs.size(); ++i) {
            if (w > 0 && runs[i].first - runs[w - 1].second <= maxGap) runs[w - 1].second = runs[i].second;
            else runs[w++] = runs[i];
        }
        runs.resize(w);
        for (auto [ra, rb] : runs) {
            if (rb - ra < minRun) continue;
            ra += inset;
            rb -= inset;
            if (rb <= ra) continue;
            Stroke s;
            s.layer = layer;
            s.pts.push_back({px0 + ra * dx, py0 + ra * dy});
            s.pts.push_back({px0 + rb * dx, py0 + rb * dy});
            out.push_back(std::move(s));
        }
    }
}

// `lightest` is the brightest tone (0..1) that still gets one layer of lines;
// every darker step adds another crossing layer.
void addHatching(const Gray& img, const Layout& L, float brush, float det, float lightest, bool invert,
                 std::vector<Stroke>& out) {
    // Spacing between parallel lines: ~25% ink per layer, a little denser with detail.
    const float S = std::max(3.5f, brush * lerp(4.6f, 3.4f, det));
    const float cell = std::max(1.f, S * 0.5f);
    int tw = std::max(4, int(L.w / cell + 0.5f)), th = std::max(4, int(L.h / cell + 0.5f));
    Gray tone = resize(img, tw, th);
    stretchContrast(tone, 0.01f, 0.99f);
    tone = gaussianBlur(tone, lerp(1.6f, 0.8f, det));
    if (invert)
        for (float& v : tone.p) v = 1.f - v;

    struct Level {
        float thr, angle, phase;
    };
    const Level levels[4] = {
        {lightest, 45.f, 0.f},
        {lightest - 0.16f, -45.f, 0.f},
        {lightest - 0.30f, 45.f, 0.5f},
        {lightest - 0.42f, -45.f, 0.5f},
    };
    for (int k = 0; k < 4; ++k) {
        if (levels[k].thr <= 0.04f) continue;
        hatchLines(tone, L, S, levels[k].angle, levels[k].phase, levels[k].thr, brush, 1 + k, out);
    }
}

void addDots(const Gray& img, const Layout& L, float brush, float det, float sh, bool invert, std::vector<Stroke>& out) {
    // One cell per pen dot, snapped to whole pixels so rows do not alias.
    float cell = std::max(1.f, std::round(brush));
    int gw = int(L.w / cell), gh = int(L.h / cell);
    while (double(gw) * double(gh) > 1.5e6) {
        cell += 1.f;
        gw = int(L.w / cell);
        gh = int(L.h / cell);
    }
    if (gw < 2 || gh < 2) return;
    Gray t = resize(img, gw, gh);
    stretchContrast(t, 0.01f, 0.99f);
    localContrast(t, 8, 2.0f, 0.25f);
    // Detail sharpens the picture before dithering.
    const float sharpen = lerp(0.f, 1.2f, det);
    if (sharpen > 0.01f) {
        Gray soft = gaussianBlur(t, 1.2f);
        for (size_t i = 0; i < t.p.size(); ++i)
            t.p[i] = std::clamp(t.p[i] + (t.p[i] - soft.p[i]) * sharpen, 0.f, 1.f);
    }
    if (invert)
        for (float& v : t.p) v = 1.f - v;
    const float gamma = lerp(1.8f, 0.75f, sh);
    for (float& v : t.p) v = std::pow(std::clamp(v, 0.f, 1.f), gamma);

    // Serpentine Floyd-Steinberg dithering.
    std::vector<uint8_t> ink(size_t(gw) * size_t(gh), 0);
    for (int y = 0; y < gh; ++y) {
        const bool ltr = (y % 2) == 0;
        const int d = ltr ? 1 : -1;
        for (int i = 0; i < gw; ++i) {
            int x = ltr ? i : gw - 1 - i;
            float old = t.at(x, y);
            float nv = old < 0.5f ? 0.f : 1.f;
            ink[size_t(y) * gw + x] = old < 0.5f;
            float err = old - nv;
            if (x + d >= 0 && x + d < gw) t.at(x + d, y) += err * (7.f / 16.f);
            if (y + 1 < gh) {
                if (x - d >= 0 && x - d < gw) t.at(x - d, y + 1) += err * (3.f / 16.f);
                t.at(x, y + 1) += err * (5.f / 16.f);
                if (x + d >= 0 && x + d < gw) t.at(x + d, y + 1) += err * (1.f / 16.f);
            }
        }
    }

    const float cx = L.w / float(gw), cy = L.h / float(gh);
    for (int y = 0; y < gh; ++y) {
        int x = 0;
        while (x < gw) {
            if (!ink[size_t(y) * gw + x]) {
                ++x;
                continue;
            }
            int a = x;
            while (x < gw && ink[size_t(y) * gw + x]) ++x;
            int b = x - 1;
            Stroke s;
            s.layer = 1;
            float yy = L.oy + (y + 0.5f) * cy;
            s.pts.push_back({L.ox + (a + 0.5f) * cx, yy});
            if (b > a) s.pts.push_back({L.ox + (b + 0.5f) * cx, yy});
            out.push_back(std::move(s));
        }
    }
}

}  // namespace

Layout fitLayout(int imgW, int imgH, float areaW, float areaH, bool stretch) {
    Layout L;
    if (imgW <= 0 || imgH <= 0 || areaW <= 0 || areaH <= 0) return L;
    if (stretch) {
        L.w = areaW;
        L.h = areaH;
        return L;
    }
    float s = std::min(areaW / float(imgW), areaH / float(imgH));
    L.w = float(imgW) * s;
    L.h = float(imgH) * s;
    L.ox = (areaW - L.w) * 0.5f;
    L.oy = (areaH - L.h) * 0.5f;
    return L;
}

bool buildDrawing(const Gray& img, float areaW, float areaH, const Params& p, Drawing& out,
                  const std::atomic<bool>* cancel) {
    out = Drawing{};
    out.width = areaW;
    out.height = areaH;
    const float brush = std::clamp(p.brush, 1.f, 64.f);
    out.brush = brush;
    if (img.empty() || areaW < 4 || areaH < 4) return true;

    const float det = float(std::clamp(p.detail, 1, 10) - 1) / 9.f;
    const float sh = float(std::clamp(p.shading, 0, 10)) / 10.f;
    const Layout L = fitLayout(img.w, img.h, areaW, areaH, p.stretch);
    if (L.w < 2 || L.h < 2) return true;

    std::vector<Stroke> strokes;
    if (p.style == Style::Outline || p.style == Style::Sketch) addOutlines(img, L, brush, det, strokes);
    if (cancelled(cancel)) return false;
    // With outlines the shading stays in the darker half so the lines keep reading.
    if (p.style == Style::Sketch && p.shading > 0)
        addHatching(img, L, brush, det, lerp(0.22f, 0.66f, sh), p.invert, strokes);
    if (p.style == Style::Hatch) addHatching(img, L, brush, det, lerp(0.40f, 0.82f, sh), p.invert, strokes);
    if (p.style == Style::Dots) addDots(img, L, brush, det, sh, p.invert, strokes);
    if (cancelled(cancel)) return false;

    orderStrokes(strokes, Pt{0, 0});
    joinStrokes(strokes, std::max(1.f, brush * 0.8f));
    out.strokes = std::move(strokes);
    return !cancelled(cancel);
}

}  // namespace dz
