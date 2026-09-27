#include "simulate.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace dz {

std::vector<Stroke> simulateFrames(const std::vector<Stroke>& strokes, const Timing& t, const FrameModel& fm) {
    std::vector<Step> steps;
    for (const Stroke& s : strokes) planStroke(s, t, steps);
    std::vector<Stroke> out;

    std::mt19937 rng(fm.seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const double frame = 1000.0 / std::max(1.0, fm.fps);
    double f = uni(rng) * frame;  // time of the next frame
    auto nextFrame = [&] {
        double dt = frame * (1.0 + fm.jitter * uni(rng));
        if (uni(rng) < fm.hitch) dt += frame * double(1 + int(uni(rng) * std::max(1, fm.hitchFrames)));
        f += dt;
    };
    bool wasDown = false;
    auto see = [&](Pt at, bool down) {
        if (down) {
            if (!wasDown) out.push_back(Stroke{{at}, 0});
            else if (dist(out.back().pts.back(), at) > 0) out.back().pts.push_back(at);
        }
        wasDown = down;
    };

    // What the mouse did, to look back in time: (time, position) and (time, button) changes.
    std::vector<std::pair<double, Pt>> moves{{-1e9, steps[0].at}};
    std::vector<std::pair<double, bool>> buttons{{-1e9, false}};
    auto at = [](const auto& hist, double t) {
        auto it = std::upper_bound(hist.begin(), hist.end(), t,
                                   [](double v, const auto& e) { return v < e.first; });
        return std::prev(it == hist.begin() ? std::next(it) : it)->second;
    };

    // Every frame looks at the state of that moment (the button and the position possibly a
    // little apart in time). A synced step starts its hold only once the game has taken it,
    // at its next frame.
    double now = 0, tb = -1e9, tp = -1e9;
    for (const Step& st : steps) {
        now += 0.05;  // same event cost as the time estimate
        if (dist(st.at, moves.back().second) > 0) moves.push_back({now, st.at});
        if (st.down != buttons.back().second) buttons.push_back({now, st.down});
        const double from = st.sync && fm.pumps ? std::max(now, f) : now;
        const double end = from + st.ms;
        while (f < end) {
            // A delay line: each frame sees things between half and all of the lag late,
            // never older than what the previous frame saw.
            tb = std::max(tb, f - fm.buttonLag * frame * (0.5 + 0.5 * uni(rng)));
            tp = std::max(tp, f - fm.posLag * frame * (0.5 + 0.5 * uni(rng)));
            see(at(moves, tp), at(buttons, tb));
            nextFrame();
        }
        now = end;
    }
    return out;
}

namespace {

// Square dilation of a binary mask by r pixels (separable max filter).
std::vector<uint8_t> dilate(const std::vector<uint8_t>& m, int w, int h, int r) {
    if (r <= 0) return m;
    std::vector<uint8_t> tmp(m.size(), 0), out(m.size(), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!m[size_t(y) * w + x]) continue;
            for (int xx = std::max(0, x - r); xx <= std::min(w - 1, x + r); ++xx) tmp[size_t(y) * w + xx] = 1;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!tmp[size_t(y) * w + x]) continue;
            for (int yy = std::max(0, y - r); yy <= std::min(h - 1, y + r); ++yy) out[size_t(yy) * w + x] = 1;
        }
    return out;
}

}  // namespace

InkDiff compareInk(const Gray& want, const Gray& got, int tolPx) {
    InkDiff d;
    if (want.w != got.w || want.h != got.h || want.w <= 0 || want.h <= 0) return d;
    const size_t n = size_t(want.w) * size_t(want.h);
    std::vector<uint8_t> a(n), b(n);
    size_t ink = 0;
    for (size_t i = 0; i < n; ++i) {
        a[i] = want.p[i] < 0.5f;
        b[i] = got.p[i] < 0.5f;
        ink += a[i];
    }
    if (!ink) return d;
    const auto da = dilate(a, want.w, want.h, tolPx), db = dilate(b, want.w, want.h, tolPx);
    size_t missing = 0, extra = 0;
    for (size_t i = 0; i < n; ++i) {
        missing += a[i] && !db[i];
        extra += b[i] && !da[i];
    }
    d.missing = double(missing) / double(ink);
    d.extra = double(extra) / double(ink);
    return d;
}

int countStrayLines(const Gray& want, const std::vector<Stroke>& seen, float minLen) {
    if (want.w <= 0 || want.h <= 0) return 0;
    std::vector<uint8_t> ink(size_t(want.w) * size_t(want.h));
    for (size_t i = 0; i < ink.size(); ++i) ink[i] = want.p[i] < 0.8f;  // thin diagonals render grey
    const std::vector<uint8_t> near = dilate(ink, want.w, want.h, 2);
    int stray = 0;
    for (const Stroke& s : seen)
        for (size_t i = 1; i < s.pts.size(); ++i) {
            const Pt a = s.pts[i - 1], b = s.pts[i];
            const float len = dist(a, b);
            if (len < minLen) continue;
            const int n = int(len);
            int off = 0;
            for (int k = 0; k <= n; ++k) {
                const int x = std::clamp(int(std::floor(a.x + (b.x - a.x) * float(k) / float(n))), 0, want.w - 1);
                const int y = std::clamp(int(std::floor(a.y + (b.y - a.y) * float(k) / float(n))), 0, want.h - 1);
                if (!near[size_t(y) * want.w + x]) ++off;
            }
            if (off * 2 > n + 1) ++stray;
        }
    return stray;
}

}  // namespace dz
