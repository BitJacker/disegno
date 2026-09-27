// Unit tests for the portable drawing engine. Run with ctest (Linux/macOS builds).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/calibrate.h"
#include "core/image.h"
#include "core/pipeline.h"
#include "core/render.h"
#include "core/simulate.h"
#include "core/strokes.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// White picture with a black disc and a dark-grey square.
dz::Gray testPicture(int w, int h) {
    dz::Gray g(w, h, 1.f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float dx = x - w * 0.35f, dy = y - h * 0.5f;
            if (dx * dx + dy * dy < (h * 0.3f) * (h * 0.3f)) g.at(x, y) = 0.f;
            if (x > w * 0.65f && x < w * 0.9f && y > h * 0.3f && y < h * 0.7f) g.at(x, y) = 0.35f;
        }
    return g;
}

void testFitLayout() {
    dz::Layout L = dz::fitLayout(200, 100, 400, 400, false);
    CHECK(near(L.w, 400) && near(L.h, 200));
    CHECK(near(L.ox, 0) && near(L.oy, 100));
    L = dz::fitLayout(100, 200, 400, 400, false);
    CHECK(near(L.w, 200) && near(L.h, 400) && near(L.ox, 100) && near(L.oy, 0));
    L = dz::fitLayout(100, 200, 300, 120, true);
    CHECK(near(L.w, 300) && near(L.h, 120) && near(L.ox, 0) && near(L.oy, 0));
}

void testSimplify() {
    std::vector<dz::Pt> pts;
    for (int i = 0; i <= 100; ++i) pts.push_back({float(i), 5.f});
    dz::simplify(pts, 0.5f);
    CHECK(pts.size() == 2);
    CHECK(near(pts.front().x, 0) && near(pts.back().x, 100));

    std::vector<dz::Pt> corner = {{0, 0}, {5, 0}, {10, 0}, {10, 5}, {10, 10}};
    dz::simplify(corner, 0.5f);
    CHECK(corner.size() == 3);
}

void testOrderAndJoin() {
    std::vector<dz::Stroke> s(4);
    s[0].pts = {{100, 0}, {110, 0}};
    s[0].layer = 1;
    s[1].pts = {{20, 0}, {10, 0}};  // will be reversed: its end is closer to the start
    s[2].pts = {{0, 0}, {5, 0}};
    s[3].pts = {{50, 50}};
    dz::orderStrokes(s, dz::Pt{0, 0});
    CHECK(s.size() == 4);
    CHECK(s[0].layer == 0 && s[1].layer == 0 && s[2].layer == 0 && s[3].layer == 1);
    CHECK(near(s[0].pts.front().x, 0));
    CHECK(near(s[1].pts.front().x, 10) && near(s[1].pts.back().x, 20));  // reversed
    CHECK(s[2].pts.size() == 1);

    std::vector<dz::Stroke> j(2);
    j[0].pts = {{0, 0}, {10, 0}};
    j[1].pts = {{10.5f, 0}, {20, 0}};
    dz::joinStrokes(j, 1.f);
    CHECK(j.size() == 1 && j[0].pts.size() == 4);

    std::vector<dz::Stroke> k(2);
    k[0].pts = {{0, 0}, {10, 0}};
    k[1].pts = {{30, 0}, {40, 0}};
    dz::joinStrokes(k, 1.f);
    CHECK(k.size() == 2);
}

void testTiming() {
    CHECK(dz::movesForSegment(10, 0) == 1);
    CHECK(dz::movesForSegment(10, 4) == 3);
    CHECK(dz::movesForSegment(0.1f, 4) == 1);
    std::vector<dz::Stroke> none;
    dz::Timing t;
    CHECK(dz::estimateSeconds(none, t) == 0);
    std::vector<dz::Stroke> one(1);
    one[0].pts = {{0, 0}, {100, 0}};
    dz::Timing fast{10, 1, 2, 2, false}, slow{5, 17, 40, 40, true};
    double a = dz::estimateSeconds(one, fast), b = dz::estimateSeconds(one, slow);
    CHECK(a > 0 && b > a);
    CHECK(near(float(dz::strokeSeconds(one[0], slow)), float(b)));
}

void testFitToTime() {
    // One long outline, many tiny shading crumbs: the outline must survive the cut.
    std::vector<dz::Stroke> s;
    dz::Stroke longLine;
    longLine.pts = {{0, 0}, {300, 0}};
    longLine.layer = 0;
    for (int i = 0; i < 50; ++i) {
        dz::Stroke crumb;
        crumb.pts = {{float(i * 5), 50}, {float(i * 5 + 2), 50}};
        crumb.layer = 2;
        s.push_back(crumb);
    }
    s.insert(s.begin() + 25, longLine);
    dz::Timing game{24, 17, 35, 35, false};
    const double all = dz::estimateSeconds(s, game);
    const double budget = all / 4;
    std::vector<dz::Stroke> fit = dz::fitToTime(s, game, budget);
    CHECK(!fit.empty() && fit.size() < s.size());
    CHECK(dz::estimateSeconds(fit, game) <= budget + 1e-9);
    bool hasLong = false;
    for (const auto& st : fit) hasLong |= st.layer == 0;
    CHECK(hasLong);
    // Order is preserved: kept crumbs stay in increasing x.
    float lastX = -1;
    bool ordered = true;
    for (const auto& st : fit)
        if (st.layer == 2) {
            if (st.pts[0].x < lastX) ordered = false;
            lastX = st.pts[0].x;
        }
    CHECK(ordered);
    // A generous budget keeps everything.
    CHECK(dz::fitToTime(s, game, all + 1).size() == s.size());
}

void testResizeAndOrientation() {
    dz::Gray g(40, 20, 0.25f);
    dz::Gray small = dz::resize(g, 10, 5);
    CHECK(small.w == 10 && small.h == 5);
    for (float v : small.p) CHECK(near(v, 0.25f));
    dz::Gray big = dz::resize(g, 80, 40);
    for (float v : big.p) CHECK(near(v, 0.25f));

    dz::Rgba img(2, 1);
    img.p = {255, 0, 0, 255, 0, 0, 255, 255};  // red, blue
    dz::Rgba r6 = dz::applyExifOrientation(img, 6);  // rotate 90 CW -> 1x2, red on top
    CHECK(r6.w == 1 && r6.h == 2);
    CHECK(r6.p[0] == 255 && r6.p[6] == 255);
    dz::Rgba r3 = dz::applyExifOrientation(img, 3);  // 180 -> blue first
    CHECK(r3.w == 2 && r3.p[2] == 255);
}

void testPipeline() {
    const dz::Gray pic = testPicture(300, 200);
    const float W = 600, H = 400;
    for (int style = 0; style < dz::kStyleCount; ++style) {
        dz::Params p;
        p.style = dz::Style(style);
        p.brush = 2;
        dz::Drawing d;
        CHECK(dz::buildDrawing(pic, W, H, p, d));
        CHECK(!d.strokes.empty());
        CHECK(near(d.width, W) && near(d.height, H));
        bool inside = true;
        for (const auto& s : d.strokes)
            for (const auto& q : s.pts)
                if (q.x < -1 || q.y < -1 || q.x > W + 1 || q.y > H + 1) inside = false;
        CHECK(inside);
        int lastLayer = -1;
        bool ordered = true;
        for (const auto& s : d.strokes) {
            if (s.layer < lastLayer) ordered = false;
            lastLayer = s.layer;
        }
        CHECK(ordered);
    }

    // Outlines trace the disc border: points near radius 0.3*H (in area coords).
    dz::Params p;
    p.style = dz::Style::Outline;
    p.brush = 1;
    dz::Drawing d;
    dz::buildDrawing(pic, W, H, p, d);
    const float cx = W * 0.35f, cy = H * 0.5f, r = H * 0.3f;
    int onCircle = 0, total = 0;
    for (const auto& s : d.strokes)
        for (const auto& q : s.pts) {
            ++total;
            float dist = std::hypot(q.x - cx, q.y - cy);
            if (std::fabs(dist - r) < 6) ++onCircle;
        }
    CHECK(total > 0 && onCircle > total / 3);

    // A blank picture gives nothing to draw.
    dz::Gray blank(100, 100, 1.f);
    dz::Drawing e;
    dz::Params ps;
    ps.style = dz::Style::Sketch;
    dz::buildDrawing(blank, 300, 300, ps, e);
    CHECK(e.strokes.empty());

    // Cancellation is honoured.
    std::atomic<bool> cancel{true};
    dz::Drawing c;
    CHECK(!dz::buildDrawing(pic, W, H, ps, c, &cancel));
}


// Two black squares on white paper, far apart.
dz::Gray twoSquares(int w, int h) {
    dz::Gray g(w, h, 1.f);
    for (int y = h / 4; y < 3 * h / 4; ++y)
        for (int x = 0; x < w; ++x)
            if ((x > w / 10 && x < 4 * w / 10) || (x > 6 * w / 10 && x < 9 * w / 10)) g.at(x, y) = 0.f;
    return g;
}

void testZigZagFill() {
    // A dark block is filled by a few long zig-zag strokes, not one stroke per line.
    const dz::Gray pic = twoSquares(400, 300);
    for (dz::Style style : {dz::Style::Lines, dz::Style::Hatch}) {
        dz::Params p;
        p.style = style;
        p.brush = 2;
        dz::Drawing d;
        dz::buildDrawing(pic, 400, 300, p, d);
        CHECK(!d.strokes.empty());
        size_t points = dz::totalPoints(d.strokes);
        CHECK(d.strokes.size() * 8 < points);  // many runs per stroke
        // No stroke may cross the white paper between or around the squares.
        bool clean = true;
        for (const auto& s : d.strokes)
            for (size_t i = 1; i < s.pts.size(); ++i) {
                const dz::Pt a = s.pts[i - 1], b = s.pts[i];
                for (int k = 1; k < 20; ++k) {
                    const float f = float(k) / 20.f;
                    const int x = int(a.x + (b.x - a.x) * f), y = int(a.y + (b.y - a.y) * f);
                    // Allow the pen to touch the edge of a square (anti-aliased border).
                    bool nearInk = false;
                    for (int dy = -3; dy <= 3 && !nearInk; ++dy)
                        for (int dx = -3; dx <= 3 && !nearInk; ++dx)
                            nearInk = pic.atClamped(x + dx, y + dy) < 0.5f;
                    if (!nearInk) clean = false;
                }
            }
        CHECK(clean);
    }
}

void testFitBuilder() {
    const dz::Gray pic = testPicture(300, 200);
    const dz::Timing game = dz::kTimingGame;
    dz::Params p;
    p.style = dz::Style::Sketch;
    p.detail = 10;
    p.brush = 1;
    dz::Drawing full;
    CHECK(dz::buildDrawingFor(pic, 900, 600, p, game, 0, full));
    const double fullSec = dz::estimateSeconds(full.strokes, game);
    CHECK(fullSec > 0 && full.coarse == 1.f && !full.trimmed);

    const double limit = fullSec / 3;
    dz::Drawing fit;
    CHECK(dz::buildDrawingFor(pic, 900, 600, p, game, limit, fit));
    CHECK(!fit.strokes.empty());
    CHECK(dz::estimateSeconds(fit.strokes, game) <= limit + 1e-6);
    CHECK(fit.coarse > 1.f || fit.trimmed);

    // A limit nothing can meet still returns something that fits (cut strokes).
    dz::Drawing tiny;
    CHECK(dz::buildDrawingFor(pic, 900, 600, p, game, 0.5, tiny));
    CHECK(dz::estimateSeconds(tiny.strokes, game) <= 0.5 + 1e-6);

    // Vertex-per-frame timing simplifies polylines (never adds points).
    std::vector<dz::Stroke> st(1);
    for (int i = 0; i <= 50; ++i) st[0].pts.push_back({float(i), float(i % 2) * 0.3f});
    dz::prepareForTiming(st, game);
    CHECK(st[0].pts.size() == 2);
}

void testPlan() {
    // Game timing: every state is held for a whole frame, and each step changes either the
    // position or the button, never both.
    const dz::Timing g = dz::kTimingGame;
    const dz::Stroke s{{{0, 0}, {50, 0}, {50, 10}}, 0};
    std::vector<dz::Step> steps;
    dz::planStroke(s, g, steps);
    CHECK(steps.size() == 5);
    if (steps.size() == 5) {
        CHECK(!steps[0].down && steps[1].down && steps[2].down && steps[3].down && !steps[4].down);
        // Rest a frame at the start, then stay still after the press and after the release.
        CHECK(near(steps[0].ms, g.moveDelayMs) && near(steps[1].ms, g.downDelayMs));
        CHECK(near(steps[2].ms, g.moveDelayMs) && near(steps[3].ms, g.moveDelayMs));
        CHECK(near(steps[4].at.x, 50) && near(steps[4].at.y, 10) && near(steps[4].ms, g.upDelayMs));
        // The game window is waited for after the press and the release only.
        CHECK(!steps[0].sync && steps[1].sync && !steps[2].sync && !steps[3].sync && steps[4].sync);
    }
    for (size_t i = 1; i < steps.size(); ++i) {
        const bool moved = dz::dist(steps[i].at, steps[i - 1].at) > 0;
        CHECK(!(moved && steps[i].down != steps[i - 1].down));
    }
    // The estimate is the sum of the steps (plus the expected waits for the game).
    double ms = 0;
    for (const auto& st : steps) ms += 0.05 + st.ms + (st.sync ? dz::kSyncWaitMs : 0.f);
    CHECK(near(float(dz::strokeSeconds(s, g)), float(ms / 1000.0)));

    // Apps: small steps, and the last one waits before the release.
    const dz::Timing f = dz::kTimingFast;
    const dz::Stroke line{{{0, 0}, {100, 0}}, 0};
    steps.clear();
    dz::planStroke(line, f, steps);
    CHECK(steps.size() == 2 + 10 + 1);
    if (steps.size() == 13) CHECK(near(steps[11].ms, f.moveDelayMs + f.upDelayMs));

    // A dot gets a one-pixel nudge while pressed.
    const dz::Stroke dot{{{5, 5}}, 0};
    steps.clear();
    dz::planStroke(dot, g, steps);
    CHECK(steps.size() == 4 && steps[2].down && near(steps[2].at.x, 6));
}

void testGameSimulation() {
    const dz::Gray pic = testPicture(300, 200);
    dz::Params p;
    p.style = dz::Style::Lines;
    p.detail = 8;
    p.brush = 1;
    const float W = 600, H = 400;
    auto check = [&](const dz::Timing& t, const dz::FrameModel& fm) {
        dz::Drawing d;
        dz::buildDrawingFor(pic, W, H, p, t, 0, d);
        dz::Gray want(int(W), int(H), 1.f), got(int(W), int(H), 1.f);
        dz::renderStrokes(want, d.strokes, 1.f, 0, 0, 1.f);
        dz::renderStrokes(got, dz::simulateFrames(d.strokes, t, fm), 1.f, 0, 0, 1.f);
        return dz::compareInk(want, got, 1);
    };
    // A steady 60 fps game sees everything; so does a 30 fps one with the slow preset.
    dz::FrameModel steady;
    dz::InkDiff a = check(dz::kTimingGame, steady);
    CHECK(a.missing < 0.001 && a.extra < 0.001);
    dz::FrameModel slow;
    slow.fps = 30;
    slow.jitter = 0.3;
    dz::InkDiff b = check(dz::kTimingGameSlow, slow);
    CHECK(b.missing < 0.01 && b.extra < 0.01);
    // A stuttering 60 fps game loses very little with the game preset...
    dz::FrameModel stutter;
    stutter.jitter = 0.3;
    stutter.hitch = 0.03;
    dz::InkDiff c = check(dz::kTimingGame, stutter);
    CHECK(c.missing < 0.05 && c.extra < 0.01);
    // ...while holding each corner for less than a frame loses a lot (the simulator notices).
    dz::InkDiff e = check(dz::Timing{0, 10, 10, 4, false}, slow);
    CHECK(e.missing > 0.2);

    // A game that freezes for up to 8 frames: waiting for it at every press and release
    // means no two strokes are ever joined by a stray line.
    p.style = dz::Style::Sketch;
    dz::FrameModel freeze;
    freeze.jitter = 0.3;
    freeze.hitch = 0.06;
    freeze.hitchFrames = 8;
    auto strays = [&](const dz::Timing& t) {
        dz::Drawing d;
        dz::buildDrawingFor(pic, W, H, p, t, 0, d);
        dz::Gray want(int(W), int(H), 1.f);
        dz::renderStrokes(want, d.strokes, 1.f, 0, 0, 1.f);
        int n = 0;
        for (uint32_t seed = 1; seed <= 20; ++seed) {
            freeze.seed = seed;
            n += dz::countStrayLines(want, dz::simulateFrames(d.strokes, t, freeze));
        }
        return n;
    };
    // Short pauses after each click, so that only the wait for the game makes the difference.
    const dz::Timing quick{0, 34, 34, 4, false, true};
    dz::Timing unsynced = quick;
    unsynced.sync = false;
    const int withSync = strays(quick), without = strays(unsynced);
    std::printf("  stray lines in a freezing game: %d synced, %d not synced\n", withSync, without);
    CHECK(withSync == 0);
    CHECK(without > withSync);

    // compareInk basics.
    dz::Gray blank(20, 20, 1.f), line(20, 20, 1.f);
    for (int x = 2; x < 18; ++x) line.at(x, 10) = 0.f;
    dz::InkDiff same = dz::compareInk(line, line, 1), none = dz::compareInk(line, blank, 1);
    CHECK(same.missing == 0 && same.extra == 0);
    CHECK(near(float(none.missing), 1.f) && none.extra == 0);
}

void testClickLag() {
    // A game that notices presses and releases up to 4 frames late but follows the mouse
    // live: leaving right after a release draws the jump to the next stroke. The game
    // presets stay still long enough after every press and release.
    const dz::Gray pic = testPicture(300, 200);
    dz::Params p;
    p.style = dz::Style::Sketch;
    p.detail = 8;
    p.brush = 1;
    const float W = 600, H = 400;
    dz::FrameModel lag;
    lag.jitter = 0.2;
    lag.buttonLag = 4;
    auto strays = [&](const dz::Timing& t) {
        dz::Drawing d;
        dz::buildDrawingFor(pic, W, H, p, t, 0, d);
        dz::Gray want(int(W), int(H), 1.f);
        dz::renderStrokes(want, d.strokes, 1.f, 0, 0, 1.f);
        int n = 0;
        for (uint32_t seed = 1; seed <= 5; ++seed) {
            lag.seed = seed;
            n += dz::countStrayLines(want, dz::simulateFrames(d.strokes, t, lag));
        }
        return n;
    };
    const int old = strays(dz::Timing{0, 34, 34, 4, false, true}), now = strays(dz::kTimingGame);
    std::printf("  stray lines in a game that sees clicks 4 frames late: %d before, %d now\n", old, now);
    CHECK(old > 0);
    CHECK(now == 0);
}

void testSpareTime() {
    // With time to spare the drawing slows down (safer) but still finishes in time.
    const dz::Gray pic = testPicture(300, 200);
    dz::Params p;
    p.style = dz::Style::Outline;
    p.brush = 1;
    const dz::Timing g = dz::kTimingGame;
    dz::Drawing free, roomy, tight;
    CHECK(dz::buildDrawingFor(pic, 600, 400, p, g, 0, free));
    CHECK(free.slow == 1.f);
    const double need = dz::estimateSeconds(free.strokes, g);
    CHECK(dz::buildDrawingFor(pic, 600, 400, p, g, need * 10, roomy));
    CHECK(roomy.slow > 1.5f && roomy.slow <= 3.f);
    CHECK(dz::estimateSeconds(roomy.strokes, dz::slowed(g, roomy.slow)) <= need * 10 + 1e-6);
    CHECK(dz::buildDrawingFor(pic, 600, 400, p, g, need * 1.2, tight));
    CHECK(tight.slow >= 1.f && tight.slow < 1.25f);
    CHECK(dz::estimateSeconds(tight.strokes, dz::slowed(g, tight.slow)) <= need * 1.2 + 1e-6);
}

void testCalibration() {
    const int w = 600, h = 400;
    const dz::CalibPlan plan = dz::planCalibration(float(w), float(h), 2.f);
    CHECK(int(plan.rows.size()) == dz::kCalibLevels);
    float lastY = -1, lastMs = -1;
    bool inside = true;
    for (const auto& r : plan.rows) {
        CHECK(r.y > lastY && r.clickMs > lastMs && r.dashes.size() >= 3);
        lastY = r.y;
        lastMs = r.clickMs;
        CHECK(r.lead.x < r.dashes.front().pts.front().x);
        for (const auto& d : r.dashes)
            for (const auto& q : d.pts) inside &= q.x >= 0 && q.x < w && q.y >= 0 && q.y < h;
    }
    CHECK(inside);

    // A fake screen: rows 0-1 joined into one line (releases seen late), row 3 with a dash
    // missing its start (press seen late), the others clean.
    std::vector<uint32_t> before(size_t(w) * h, 0xFFF5F0F0u), after = before;
    auto line = [&](float x0, float x1, float y) {
        for (int x = int(x0); x <= int(x1); ++x)
            for (int dy = 0; dy < 2; ++dy) after[size_t(int(y) + dy) * w + size_t(x)] = 0xFF101010u;
    };
    for (size_t i = 0; i < plan.rows.size(); ++i) {
        const auto& r = plan.rows[i];
        for (size_t d = 0; d < r.dashes.size(); ++d) {
            float xa = r.dashes[d].pts.front().x;
            const float xb = r.dashes[d].pts.back().x;
            if (i == 3 && d == 2) xa = (xa + xb) / 2;
            line(xa, xb, r.y);
            if (i < 2 && d + 1 < r.dashes.size()) line(xb, r.dashes[d + 1].pts.front().x, r.y);
        }
    }
    const dz::CalibResult res = dz::analyzeCalibration(plan, before.data(), after.data(), w, h);
    CHECK(res.sawInk && res.rows.size() == plan.rows.size());
    if (res.rows.size() == 6) {
        CHECK(!res.rows[0].clean && !res.rows[1].clean && res.rows[2].clean);
        CHECK(!res.rows[3].clean && res.rows[4].clean && res.rows[5].clean);
    }
    CHECK(res.pick == 4);
    CHECK(!dz::analyzeCalibration(plan, before.data(), before.data(), w, h).sawInk);
    CHECK(dz::planCalibration(50, 40, 1).rows.empty());
    // Slow games get longer rests on corners too, within limits.
    CHECK(dz::calibTiming(20).moveDelayMs == 34.f && dz::calibTiming(150).moveDelayMs == 75.f);
}

void testRender() {
    dz::Gray canvas(50, 50, 1.f);
    std::vector<dz::Stroke> s(1);
    s[0].pts = {{5, 25}, {45, 25}};
    dz::renderStrokes(canvas, s, 1.f, 0, 0, 2.f);
    CHECK(canvas.at(25, 25) < 0.1f);
    CHECK(canvas.at(25, 5) > 0.99f);
}

}  // namespace

int main() {
    testFitLayout();
    testSimplify();
    testOrderAndJoin();
    testTiming();
    testFitToTime();
    testResizeAndOrientation();
    testPipeline();
    testZigZagFill();
    testFitBuilder();
    testPlan();
    testGameSimulation();
    testClickLag();
    testSpareTime();
    testCalibration();
    testRender();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all core tests passed\n");
    return 0;
}
