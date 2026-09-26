#include "image.h"

#include <algorithm>
#include <cmath>

namespace dz {

float Gray::atClamped(int x, int y) const {
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    return at(x, y);
}

float Gray::sample(float x, float y) const {
    if (empty()) return 1.f;
    x = std::clamp(x, 0.f, float(w - 1));
    y = std::clamp(y, 0.f, float(h - 1));
    int x0 = int(x), y0 = int(y);
    int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    float fx = x - float(x0), fy = y - float(y0);
    float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
    float b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
    return a + (b - a) * fy;
}

Gray toGray(const Rgba& img) {
    Gray g(img.w, img.h);
    const uint8_t* s = img.p.data();
    for (size_t i = 0, n = size_t(img.w) * size_t(img.h); i < n; ++i, s += 4) {
        float lum = (0.2126f * s[0] + 0.7152f * s[1] + 0.0722f * s[2]) / 255.f;
        float a = s[3] / 255.f;
        g.p[i] = lum * a + (1.f - a);
    }
    return g;
}

namespace {

struct Tap {
    int idx;
    float w;
};

// Per output sample: [offsets[i], offsets[i+1]) indexes into taps.
struct Kernel {
    std::vector<int> offsets;
    std::vector<Tap> taps;
};

Kernel makeKernel(int srcLen, int dstLen) {
    Kernel k;
    k.offsets.reserve(size_t(dstLen) + 1);
    double scale = double(srcLen) / double(dstLen);
    double support = std::max(1.0, scale);
    for (int i = 0; i < dstLen; ++i) {
        k.offsets.push_back(int(k.taps.size()));
        double c = (i + 0.5) * scale - 0.5;
        int a = int(std::ceil(c - support));
        int b = int(std::floor(c + support));
        double sum = 0;
        size_t first = k.taps.size();
        for (int j = a; j <= b; ++j) {
            double wt = 1.0 - std::fabs(j - c) / support;
            if (wt <= 0) continue;
            k.taps.push_back({std::clamp(j, 0, srcLen - 1), float(wt)});
            sum += wt;
        }
        if (sum <= 0) {
            k.taps.resize(first);
            k.taps.push_back({std::clamp(int(std::lround(c)), 0, srcLen - 1), 1.f});
        } else {
            for (size_t t = first; t < k.taps.size(); ++t) k.taps[t].w = float(k.taps[t].w / sum);
        }
    }
    k.offsets.push_back(int(k.taps.size()));
    return k;
}

// Resamples an interleaved float buffer with `ch` channels.
std::vector<float> resampleChannels(const std::vector<float>& src, int w, int h, int ch, int nw, int nh) {
    Kernel kx = makeKernel(w, nw);
    Kernel ky = makeKernel(h, nh);
    std::vector<float> tmp(size_t(nw) * size_t(h) * ch, 0.f);
    for (int y = 0; y < h; ++y) {
        const float* row = &src[size_t(y) * w * ch];
        float* out = &tmp[size_t(y) * nw * ch];
        for (int x = 0; x < nw; ++x) {
            for (int t = kx.offsets[x]; t < kx.offsets[x + 1]; ++t) {
                const Tap& tp = kx.taps[t];
                for (int c = 0; c < ch; ++c) out[x * ch + c] += row[tp.idx * ch + c] * tp.w;
            }
        }
    }
    std::vector<float> dst(size_t(nw) * size_t(nh) * ch, 0.f);
    for (int y = 0; y < nh; ++y) {
        float* out = &dst[size_t(y) * nw * ch];
        for (int t = ky.offsets[y]; t < ky.offsets[y + 1]; ++t) {
            const Tap& tp = ky.taps[t];
            const float* row = &tmp[size_t(tp.idx) * nw * ch];
            for (int i = 0; i < nw * ch; ++i) out[i] += row[i] * tp.w;
        }
    }
    return dst;
}

}  // namespace

Gray resize(const Gray& src, int nw, int nh) {
    nw = std::max(1, nw);
    nh = std::max(1, nh);
    if (src.empty()) return Gray(nw, nh, 1.f);
    if (nw == src.w && nh == src.h) return src;
    Gray out;
    out.w = nw;
    out.h = nh;
    out.p = resampleChannels(src.p, src.w, src.h, 1, nw, nh);
    return out;
}

Rgba resize(const Rgba& src, int nw, int nh) {
    nw = std::max(1, nw);
    nh = std::max(1, nh);
    Rgba out(nw, nh);
    if (src.empty()) return out;
    // Premultiply so transparent pixels do not bleed their colour.
    std::vector<float> f(size_t(src.w) * size_t(src.h) * 4);
    for (size_t i = 0, n = size_t(src.w) * size_t(src.h); i < n; ++i) {
        float a = src.p[i * 4 + 3] / 255.f;
        f[i * 4 + 0] = src.p[i * 4 + 0] * a;
        f[i * 4 + 1] = src.p[i * 4 + 1] * a;
        f[i * 4 + 2] = src.p[i * 4 + 2] * a;
        f[i * 4 + 3] = a;
    }
    std::vector<float> r = resampleChannels(f, src.w, src.h, 4, nw, nh);
    for (size_t i = 0, n = size_t(nw) * size_t(nh); i < n; ++i) {
        float a = r[i * 4 + 3];
        float inv = a > 1e-5f ? 1.f / a : 0.f;
        for (int c = 0; c < 3; ++c)
            out.p[i * 4 + c] = uint8_t(std::min(255.f, std::max(0.f, r[i * 4 + c] * inv) + 0.5f));
        out.p[i * 4 + 3] = uint8_t(std::min(255.f, std::max(0.f, a * 255.f) + 0.5f));
    }
    return out;
}

Gray gaussianBlur(const Gray& src, float sigma) {
    if (sigma < 0.3f || src.empty()) return src;
    int r = std::max(1, int(std::ceil(sigma * 3.f)));
    std::vector<float> k(size_t(2 * r + 1));
    float sum = 0;
    for (int i = -r; i <= r; ++i) {
        k[size_t(i + r)] = std::exp(-(i * i) / (2.f * sigma * sigma));
        sum += k[size_t(i + r)];
    }
    for (float& v : k) v /= sum;

    const int w = src.w, h = src.h;
    Gray tmp(w, h), out(w, h);
    for (int y = 0; y < h; ++y) {
        const float* row = &src.p[size_t(y) * w];
        float* o = &tmp.p[size_t(y) * w];
        for (int x = 0; x < w; ++x) {
            float acc = 0;
            if (x >= r && x < w - r) {
                const float* s = row + x - r;
                for (int i = 0; i <= 2 * r; ++i) acc += s[i] * k[size_t(i)];
            } else {
                for (int i = -r; i <= r; ++i) acc += row[std::clamp(x + i, 0, w - 1)] * k[size_t(i + r)];
            }
            o[x] = acc;
        }
    }
    std::vector<float> col(static_cast<size_t>(h));
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) col[size_t(y)] = tmp.p[size_t(y) * w + x];
        for (int y = 0; y < h; ++y) {
            float acc = 0;
            for (int i = -r; i <= r; ++i) acc += col[size_t(std::clamp(y + i, 0, h - 1))] * k[size_t(i + r)];
            out.p[size_t(y) * w + x] = acc;
        }
    }
    return out;
}

void stretchContrast(Gray& g, float lowFrac, float highFrac) {
    if (g.empty()) return;
    const int bins = 4096;
    std::vector<size_t> hist(bins, 0);
    for (float v : g.p) hist[size_t(std::clamp(int(v * (bins - 1) + 0.5f), 0, bins - 1))]++;
    const double n = double(g.p.size());
    auto valueAt = [&](double frac) {
        double target = frac * n, acc = 0;
        for (int i = 0; i < bins; ++i) {
            acc += double(hist[size_t(i)]);
            if (acc >= target) return float(i) / float(bins - 1);
        }
        return 1.f;
    };
    float lo = valueAt(lowFrac), hi = valueAt(highFrac);
    if (hi - lo < 0.02f) return;  // flat image: leave it alone
    float inv = 1.f / (hi - lo);
    for (float& v : g.p) v = std::clamp((v - lo) * inv, 0.f, 1.f);
}

void localContrast(Gray& g, int tiles, float clipLimit, float amount) {
    if (g.empty() || amount <= 0.f) return;
    const int bins = 256;
    const int w = g.w, h = g.h;
    int tilesX = tiles, tilesY = tiles;
    if (w >= h) tilesY = std::max(1, int(std::lround(double(tiles) * h / w)));
    else tilesX = std::max(1, int(std::lround(double(tiles) * w / h)));
    const float tw = float(w) / tilesX, th = float(h) / tilesY;

    std::vector<float> lut(size_t(tilesX) * tilesY * bins);
    std::vector<float> hist(bins);
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            std::fill(hist.begin(), hist.end(), 0.f);
            int x0 = int(tx * tw), x1 = std::min(w, int((tx + 1) * tw));
            int y0 = int(ty * th), y1 = std::min(h, int((ty + 1) * th));
            float count = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    hist[size_t(std::clamp(int(g.at(x, y) * (bins - 1) + 0.5f), 0, bins - 1))] += 1.f;
                    count += 1.f;
                }
            if (count <= 0) count = 1;
            float limit = std::max(1.f, clipLimit * count / bins);
            float excess = 0;
            for (float& v : hist)
                if (v > limit) {
                    excess += v - limit;
                    v = limit;
                }
            float add = excess / bins;
            float acc = 0;
            float* L = &lut[(size_t(ty) * tilesX + tx) * bins];
            for (int i = 0; i < bins; ++i) {
                acc += hist[size_t(i)] + add;
                L[i] = acc / count;
            }
        }
    }
    for (int y = 0; y < h; ++y) {
        float fy = (y + 0.5f) / th - 0.5f;
        int iy0 = int(std::floor(fy));
        float ay = std::clamp(fy - iy0, 0.f, 1.f);
        int iy1 = std::clamp(iy0 + 1, 0, tilesY - 1);
        iy0 = std::clamp(iy0, 0, tilesY - 1);
        for (int x = 0; x < w; ++x) {
            float fx = (x + 0.5f) / tw - 0.5f;
            int ix0 = int(std::floor(fx));
            float ax = std::clamp(fx - ix0, 0.f, 1.f);
            int ix1 = std::clamp(ix0 + 1, 0, tilesX - 1);
            ix0 = std::clamp(ix0, 0, tilesX - 1);
            float v = g.at(x, y);
            int b = std::clamp(int(v * (bins - 1) + 0.5f), 0, bins - 1);
            float m00 = lut[(size_t(iy0) * tilesX + ix0) * bins + b];
            float m01 = lut[(size_t(iy0) * tilesX + ix1) * bins + b];
            float m10 = lut[(size_t(iy1) * tilesX + ix0) * bins + b];
            float m11 = lut[(size_t(iy1) * tilesX + ix1) * bins + b];
            float m = (m00 * (1 - ax) + m01 * ax) * (1 - ay) + (m10 * (1 - ax) + m11 * ax) * ay;
            g.at(x, y) = v + (m - v) * amount;
        }
    }
}

float quantile(std::vector<float>& values, float q) {
    if (values.empty()) return 0.f;
    size_t k = size_t(std::clamp(q, 0.f, 1.f) * float(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(k), values.end());
    return values[k];
}

Rgba applyExifOrientation(const Rgba& img, int o) {
    if (o < 2 || o > 8 || img.empty()) return img;
    const int W = img.w, H = img.h;
    const bool swap = o >= 5;
    Rgba out(swap ? H : W, swap ? W : H);
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            int sx = x, sy = y;
            switch (o) {
                case 2: sx = W - 1 - x; sy = y; break;
                case 3: sx = W - 1 - x; sy = H - 1 - y; break;
                case 4: sx = x; sy = H - 1 - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = y; sy = H - 1 - x; break;
                case 7: sx = W - 1 - y; sy = H - 1 - x; break;
                case 8: sx = W - 1 - y; sy = x; break;
            }
            const uint8_t* s = &img.p[(size_t(sy) * W + sx) * 4];
            uint8_t* d = &out.p[(size_t(y) * out.w + x) * 4];
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    return out;
}

}  // namespace dz
