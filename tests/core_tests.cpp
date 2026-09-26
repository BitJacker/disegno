// Unit tests for the portable drawing engine. Run with ctest (Linux/macOS builds).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/image.h"
#include "core/pipeline.h"
#include "core/render.h"
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
    testRender();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all core tests passed\n");
    return 0;
}
