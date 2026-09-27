#include "calibrate.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dz {

namespace {

// Pixel centre of the pixel that contains v (the engine draws at floor(v)).
float centre(float v) { return std::floor(v) + 0.5f; }

}  // namespace

CalibPlan planCalibration(float areaW, float areaH, float brush) {
    CalibPlan plan;
    brush = std::max(1.f, brush);
    const float mx = std::max(8.f, areaW * 0.05f), my = std::max(8.f, areaH * 0.06f);
    const float lead = std::max(14.f, brush * 4.f);  // between the waiting point and the first dash
    const float width = areaW - 2.f * mx - lead;
    const float spacing = (areaH - 2.f * my) / float(kCalibLevels);
    if (width < 60.f || spacing < std::max(10.f, brush * 4.f)) return plan;

    const int dashes = 6;
    const float len = std::min(80.f, width / (float(dashes) + 0.6f * float(dashes - 1)));
    const float gap = len * 0.6f;
    plan.band = std::max(2.f, brush * 0.5f + 2.f);
    for (int i = 0; i < kCalibLevels; ++i) {
        CalibRow r;
        r.y = centre(my + (float(i) + 0.5f) * spacing);
        r.clickMs = kCalibClickMs[i];
        r.lead = {centre(mx), r.y};
        float x = mx + lead;
        for (int d = 0; d < dashes; ++d) {
            Stroke s;
            s.pts = {{centre(x), r.y}, {centre(x + len), r.y}};
            r.dashes.push_back(std::move(s));
            x += len + gap;
        }
        plan.rows.push_back(std::move(r));
    }
    return plan;
}

Timing calibTiming(float clickMs) {
    // Games that notice clicks late tend to be slow all round: longer rests on corners too.
    return Timing{0.f, std::clamp(clickMs * 0.5f, 34.f, 80.f), clickMs, clickMs, false, true};
}

CalibResult analyzeCalibration(const CalibPlan& plan, const uint32_t* before, const uint32_t* after, int w, int h) {
    CalibResult res;
    if (!before || !after || w <= 0 || h <= 0) return res;
    std::vector<uint8_t> ink(size_t(w) * size_t(h));
    size_t inked = 0;
    for (size_t i = 0; i < ink.size(); ++i) {
        int d = 0;
        for (int sh = 0; sh < 24; sh += 8)
            d = std::max(d, std::abs(int((before[i] >> sh) & 255u) - int((after[i] >> sh) & 255u)));
        ink[i] = d > 48;
        inked += ink[i];
    }
    res.sawInk = inked >= 20;

    const float band = plan.band;
    auto columnInked = [&](int x, float yc) {
        const int y0 = std::max(0, int(std::floor(yc - band))), y1 = std::min(h - 1, int(std::floor(yc + band)));
        for (int y = y0; y <= y1; ++y)
            if (ink[size_t(y) * size_t(w) + size_t(x)]) return true;
        return false;
    };
    // Share of the columns between x0 and x1 with ink near the row's line.
    auto share = [&](float x0, float x1, float yc) {
        int n = 0, hit = 0;
        for (int x = std::max(0, int(std::ceil(x0))); x <= std::min(w - 1, int(std::floor(x1))); ++x) {
            ++n;
            hit += columnInked(x, yc);
        }
        return n ? double(hit) / double(n) : 0.0;
    };

    const float trimEnds = std::max(2.f, band - 1.f), gapEnds = band + 2.f;
    for (const CalibRow& r : plan.rows) {
        CalibRowResult rr;
        rr.coverage = 1;
        for (size_t d = 0; d < r.dashes.size(); ++d) {
            const float xa = r.dashes[d].pts.front().x, xb = r.dashes[d].pts.back().x;
            rr.coverage = std::min(rr.coverage, share(xa + trimEnds, xb - trimEnds, r.y));
            if (d + 1 < r.dashes.size())
                rr.stray = std::max(rr.stray, share(xb + gapEnds, r.dashes[d + 1].pts.front().x - gapEnds, r.y));
        }
        rr.clean = rr.coverage >= 0.75 && rr.stray <= 0.2;
        res.rows.push_back(rr);
    }
    for (int i = int(res.rows.size()) - 1; i >= 0 && res.rows[size_t(i)].clean; --i) res.pick = i;
    return res;
}

}  // namespace dz
