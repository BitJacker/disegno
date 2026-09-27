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
// Line detection and thinning
// ---------------------------------------------------------------------------

// Dark thin features and the dark side of strong boundaries, found with a difference of
// Gaussians. Lines drawn in the picture give one centre line (not two edges, as Canny
// does), which looks better and costs half the mouse travel. Hysteresis keeps lines
// connected. Returns a binary mask with a cleared border.
std::vector<uint8_t> dogLines(const Gray& src, float sigma, float det) {
    const int w = src.w, h = src.h;
    std::vector<uint8_t> out(size_t(w) * size_t(h), 0);
    if (w < 3 || h < 3) return out;
    const Gray a = gaussianBlur(src, sigma);
    const Gray b = gaussianBlur(src, sigma * 1.6f);
    std::vector<float> d(size_t(w) * size_t(h));
    std::vector<float> neg;
    neg.reserve(d.size() / 2);
    for (size_t i = 0; i < d.size(); ++i) {
        d[i] = a.p[i] - b.p[i];
        if (d[i] < -0.004f) neg.push_back(-d[i]);
    }
    if (neg.empty()) return out;
    const float strong = std::max(0.018f, quantile(neg, lerp(0.84f, 0.55f, det)));
    const float weak = strong * 0.5f;
    std::vector<size_t> stack;
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const size_t i = size_t(y) * w + x;
            if (out[i] || d[i] > -strong) continue;
            out[i] = 1;
            stack.push_back(i);
            while (!stack.empty()) {
                const size_t j = stack.back();
                stack.pop_back();
                const int jx = int(j % size_t(w)), jy = int(j / size_t(w));
                for (int k = 0; k < 8; ++k) {
                    const int nx = jx + DX[k], ny = jy + DY[k];
                    if (nx < 1 || ny < 1 || nx >= w - 1 || ny >= h - 1) continue;
                    const size_t n = size_t(ny) * w + nx;
                    if (!out[n] && d[n] <= -weak) {
                        out[n] = 1;
                        stack.push_back(n);
                    }
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
    std::vector<uint8_t> edges = dogLines(g, lerp(1.9f, 1.0f, det) * sizeF, det);
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

// ---------------------------------------------------------------------------
// Parallel-line shading (hatching and the "Righe" line screen)
// ---------------------------------------------------------------------------

// One stretch of ink along a scan line; `a` is the end with the smaller line parameter.
struct Run {
    Pt a, b;
    bool used = false;
};

// Samples the tone map at a point in area coordinates.
struct ToneMap {
    const Gray& tone;
    const Layout& L;
    float cw, ch;
    ToneMap(const Gray& t, const Layout& l) : tone(t), L(l), cw(l.w / float(t.w)), ch(l.h / float(t.h)) {}
    float at(float x, float y) const { return tone.sample((x - L.ox) / cw - 0.5f, (y - L.oy) / ch - 0.5f); }
};

// Finds the ink runs along parallel lines at `angleDeg`, `S` apart. Line i is inked where the
// tone is below thr[i % thr.size()]. Runs shorter than minRun are dropped, gaps shorter
// than maxGap are bridged, and both ends are pulled in by `inset`.
std::vector<std::vector<Run>> scanRuns(const ToneMap& tm, float S, float angleDeg, float phase,
                                       const std::vector<float>& thr, float minRun, float maxGap, float inset,
                                       std::vector<float>& lineThr) {
    const Layout& L = tm.L;
    const float a = angleDeg * kPi / 180.f;
    const float dx = std::cos(a), dy = std::sin(a);
    const float nx = -dy, ny = dx;
    const float x0 = L.ox, y0 = L.oy, x1 = L.ox + L.w, y1 = L.oy + L.h;
    const float cs[4] = {x0 * nx + y0 * ny, x1 * nx + y0 * ny, x0 * nx + y1 * ny, x1 * nx + y1 * ny};
    const float cmin = *std::min_element(cs, cs + 4), cmax = *std::max_element(cs, cs + 4);
    const float step = std::max(0.5f, std::min(tm.cw, tm.ch) * 0.5f);
    const float hyst = 0.025f;

    std::vector<std::vector<Run>> lines;
    lineThr.clear();
    std::vector<std::pair<float, float>> runs;
    int index = 0;
    for (float c = cmin + S * (0.5f + phase); c < cmax; c += S, ++index) {
        const float t0 = thr[size_t(index) % thr.size()];
        lines.emplace_back();
        lineThr.push_back(t0);
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
        if (t0 <= 0.f || !clip(px0, dx, x0, x1) || !clip(py0, dy, y0, y1) || tmax - tmin < 1.f) continue;

        runs.clear();
        bool on = false;
        float start = 0;
        for (float t = tmin; t <= tmax; t += step) {
            const float v = tm.at(px0 + t * dx, py0 + t * dy);
            if (!on && v < t0) {
                on = true;
                start = t;
            } else if (on && v > t0 + hyst) {
                on = false;
                runs.emplace_back(start, t - step);
            }
        }
        if (on) runs.emplace_back(start, tmax);

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
            if (rb < ra) ra = rb = (ra + rb) * 0.5f;
            lines.back().push_back(Run{{px0 + ra * dx, py0 + ra * dy}, {px0 + rb * dx, py0 + rb * dy}});
        }
    }
    return lines;
}

// Links runs of neighbouring lines into zig-zag strokes, so the pen stays down across a
// dark region instead of lifting for every line. A link is only drawn where the picture
// is dark enough (it never cuts through light areas), which keeps it invisible.
void chainRuns(std::vector<std::vector<Run>>& lines, const std::vector<float>& lineThr, const ToneMap& tm,
               float maxLink, int maxSkip, int layer, std::vector<Stroke>& out) {
    const float sampleStep = std::max(1.f, std::min(tm.cw, tm.ch));
    auto safe = [&](const Pt& p, const Pt& q, float thr) {
        const float len = dist(p, q);
        const int n = std::max(2, int(len / sampleStep) + 1);
        for (int k = 1; k < n; ++k) {
            const float f = float(k) / float(n);
            if (tm.at(p.x + (q.x - p.x) * f, p.y + (q.y - p.y) * f) > thr) return false;
        }
        return true;
    };
    const int nLines = int(lines.size());
    for (int i = 0; i < nLines; ++i) {
        for (Run& first : lines[size_t(i)]) {
            if (first.used) continue;
            first.used = true;
            Stroke s;
            s.layer = layer;
            s.pts = {first.a, first.b};
            if (dist(first.a, first.b) < 0.01f) s.pts.pop_back();
            Pt end = first.b;
            bool endIsB = true;
            int li = i;
            for (;;) {
                Run* best = nullptr;
                int bestLine = -1;
                for (int lj = li + 1; lj <= std::min(li + maxSkip, nLines - 1) && !best; ++lj) {
                    float bestD = maxLink;
                    const float linkThr = std::max(lineThr[size_t(li)], lineThr[size_t(lj)]) + 0.06f;
                    for (Run& c : lines[size_t(lj)]) {
                        if (c.used) continue;
                        const Pt& near = endIsB ? c.b : c.a;
                        const float d = dist(end, near);
                        if (d < bestD && safe(end, near, linkThr)) {
                            bestD = d;
                            best = &c;
                            bestLine = lj;
                        }
                    }
                }
                if (!best) break;
                best->used = true;
                if (endIsB) {
                    s.pts.push_back(best->b);
                    if (dist(best->a, best->b) >= 0.01f) s.pts.push_back(best->a);
                    end = best->a;
                } else {
                    s.pts.push_back(best->a);
                    if (dist(best->a, best->b) >= 0.01f) s.pts.push_back(best->b);
                    end = best->b;
                }
                endIsB = !endIsB;
                li = bestLine;
            }
            out.push_back(std::move(s));
        }
    }
}

Gray toneMap(const Gray& img, const Layout& L, float cell, float blur, bool invert) {
    int tw = std::max(4, int(L.w / cell + 0.5f)), th = std::max(4, int(L.h / cell + 0.5f));
    Gray tone = resize(img, tw, th);
    stretchContrast(tone, 0.01f, 0.99f);
    tone = gaussianBlur(tone, blur);
    if (invert)
        for (float& v : tone.p) v = 1.f - v;
    return tone;
}

// `lightest` is the brightest tone (0..1) that still gets one layer of lines;
// every darker step adds another crossing layer. `geom` sets the line spacing.
void addHatching(const Gray& img, const Layout& L, float brush, float geom, float det, float lightest, bool invert,
                 std::vector<Stroke>& out) {
    // Spacing between parallel lines: ~25% ink per layer, a little denser with detail.
    const float S = std::max(3.5f, geom * lerp(4.6f, 3.4f, det));
    const Gray tone = toneMap(img, L, std::max(1.f, S * 0.5f), lerp(1.6f, 0.8f, det), invert);
    const ToneMap tm(tone, L);

    struct Level {
        float thr, angle, phase;
    };
    const Level levels[4] = {
        {lightest, 45.f, 0.f},
        {lightest - 0.16f, -45.f, 0.f},
        {lightest - 0.30f, 45.f, 0.5f},
        {lightest - 0.42f, -45.f, 0.5f},
    };
    std::vector<float> lineThr;
    for (int k = 0; k < 4; ++k) {
        if (levels[k].thr <= 0.04f) continue;
        auto lines = scanRuns(tm, S, levels[k].angle, levels[k].phase, {levels[k].thr},
                              std::max(brush * 1.5f, S * 0.9f), S * 0.5f, brush * 0.5f, lineThr);
        chainRuns(lines, lineThr, tm, S * 2.2f, 1, 1 + k, out);
    }
}

// Photo made of horizontal lines: every row is inked where the picture is darker than
// the row's threshold; thresholds cycle down the rows so darker tones get more rows.
void addLineScreen(const Gray& img, const Layout& L, float brush, float geom, float det, float sh, bool invert,
                   std::vector<Stroke>& out) {
    const float R = std::max(1.f, geom * lerp(1.8f, 1.0f, det));
    const Gray tone = toneMap(img, L, std::max(1.f, R * 0.5f), lerp(1.2f, 0.6f, det), invert);
    const ToneMap tm(tone, L);
    const float maxT = lerp(0.55f, 0.97f, sh);
    // 1-D ordered dither: rows 0,2,1,3 of every group of four.
    std::vector<float> thr = {maxT * 0.125f, maxT * 0.625f, maxT * 0.375f, maxT * 0.875f};
    std::vector<float> lineThr;
    auto lines = scanRuns(tm, R, 0.f, 0.f, thr, std::max(brush, R * 0.8f), R * 0.6f, brush * 0.5f, lineThr);
    chainRuns(lines, lineThr, tm, R * 3.6f, 3, 1, out);
}

void addDots(const Gray& img, const Layout& L, float geom, float det, float sh, bool invert, std::vector<Stroke>& out) {
    // One cell per pen dot. Dense grids snap to whole pixels so rows do not alias; sparse
    // ones (fitting a time limit) keep the exact size so the time is used to the full.
    float cell = geom < 3.f ? std::max(1.f, std::round(geom)) : geom;
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
    // Spacing/resolution follow the pen size; `coarse` widens them to save time.
    const float geom = brush * std::clamp(p.coarse, 1.f, 32.f);
    const Layout L = fitLayout(img.w, img.h, areaW, areaH, p.stretch);
    if (L.w < 2 || L.h < 2) return true;

    std::vector<Stroke> strokes;
    if (p.style == Style::Outline || p.style == Style::Sketch) addOutlines(img, L, geom, det, strokes);
    if (cancelled(cancel)) return false;
    // With outlines the shading stays in the darker half so the lines keep reading.
    if (p.style == Style::Sketch && p.shading > 0)
        addHatching(img, L, brush, geom, det, lerp(0.22f, 0.66f, sh), p.invert, strokes);
    if (p.style == Style::Hatch) addHatching(img, L, brush, geom, det, lerp(0.40f, 0.82f, sh), p.invert, strokes);
    if (p.style == Style::Dots) addDots(img, L, geom, det, sh, p.invert, strokes);
    if (p.style == Style::Lines) addLineScreen(img, L, brush, geom, det, sh, p.invert, strokes);
    if (cancelled(cancel)) return false;

    orderStrokes(strokes, Pt{0, 0});
    joinStrokes(strokes, std::max(1.f, brush * 0.8f));
    out.strokes = std::move(strokes);
    return !cancelled(cancel);
}

namespace {

// With time to spare, every pause may get up to this many times longer (never beyond
// kMaxPauseMs): games notice presses, releases and corners more reliably.
constexpr double kMaxSlow = 3.0;
constexpr float kMaxPauseMs = 250.f;

// How much slower the drawing can go and still take at most `maxSeconds`.
float spareSlowdown(const std::vector<Stroke>& strokes, const Timing& t, double maxSeconds) {
    if (maxSeconds <= 0 || strokes.empty()) return 1.f;
    // The time grows linearly with the pauses: a*factor + b (b = waits that do not scale).
    const double one = estimateSeconds(strokes, t), two = estimateSeconds(strokes, slowed(t, 2.f));
    const double a = two - one, b = one - a;
    if (a <= 0 || one >= maxSeconds) return 1.f;
    const float longest = std::max({t.moveDelayMs, t.downDelayMs, t.upDelayMs, 1.f});
    const double cap = std::min(kMaxSlow, double(kMaxPauseMs / longest));
    return float(std::clamp((maxSeconds - b) / a, 1.0, std::max(1.0, cap)));
}

}  // namespace

bool buildDrawingFor(const Gray& img, float areaW, float areaH, const Params& p, const Timing& t, double maxSeconds,
                     Drawing& out, const std::atomic<bool>* cancel) {
    auto build = [&](float coarse, Drawing& d, double& sec) {
        Params q = p;
        q.coarse = coarse;
        if (!buildDrawing(img, areaW, areaH, q, d, cancel)) return false;
        prepareForTiming(d.strokes, t);
        d.coarse = coarse;
        d.fullCount = d.strokes.size();
        sec = estimateSeconds(d.strokes, t);
        return true;
    };
    double sec = 0;
    if (!build(1.f, out, sec)) return false;
    if (maxSeconds <= 0) return true;
    if (sec <= maxSeconds) {
        out.slow = spareSlowdown(out.strokes, t, maxSeconds);
        return true;
    }

    // Too slow: find the least coarse version that fits (time falls as coarseness grows).
    const float maxCoarse = 16.f;
    float lo = 1.f, hi = 2.f;
    Drawing fit;
    double fitSec = 0;
    for (;;) {
        if (!build(hi, fit, fitSec)) return false;
        if (fitSec <= maxSeconds || hi >= maxCoarse) break;
        lo = hi;
        hi = std::min(maxCoarse, hi * 2.f);
    }
    if (fitSec <= maxSeconds) {
        for (int it = 0; it < 8 && hi / lo > 1.03f; ++it) {
            const float mid = std::sqrt(lo * hi);
            Drawing d;
            double dsec = 0;
            if (!build(mid, d, dsec)) return false;
            if (dsec <= maxSeconds) {
                hi = mid;
                fit = std::move(d);
            } else {
                lo = mid;
            }
        }
        out = std::move(fit);
        out.slow = spareSlowdown(out.strokes, t, maxSeconds);
        return true;
    }
    // Even the coarsest version is too slow: keep its most important strokes.
    out = std::move(fit);
    out.strokes = fitToTime(out.strokes, t, maxSeconds);
    out.trimmed = true;
    return !cancelled(cancel);
}

}  // namespace dz
