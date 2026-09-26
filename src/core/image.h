// Portable image helpers used by the drawing pipeline (no platform headers).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dz {

// Single-channel float image. 0 = black (ink), 1 = white (paper).
struct Gray {
    int w = 0;
    int h = 0;
    std::vector<float> p;

    Gray() = default;
    Gray(int width, int height, float v = 0.f)
        : w(width), h(height), p(size_t(width) * size_t(height), v) {}

    bool empty() const { return w <= 0 || h <= 0; }
    float& at(int x, int y) { return p[size_t(y) * size_t(w) + size_t(x)]; }
    float at(int x, int y) const { return p[size_t(y) * size_t(w) + size_t(x)]; }
    float atClamped(int x, int y) const;
    // Bilinear sample; pixel centers sit on integer coordinates.
    float sample(float x, float y) const;
};

// 8-bit RGBA image with straight (non-premultiplied) alpha.
struct Rgba {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> p;  // w * h * 4 bytes, R G B A

    Rgba() = default;
    Rgba(int width, int height) : w(width), h(height), p(size_t(width) * size_t(height) * 4, 255) {}
    bool empty() const { return w <= 0 || h <= 0; }
};

// Luminance composited over a white background.
Gray toGray(const Rgba& img);

// High quality resample: tent filter widened when shrinking (antialiased).
Gray resize(const Gray& src, int nw, int nh);
Rgba resize(const Rgba& src, int nw, int nh);

Gray gaussianBlur(const Gray& src, float sigma);

// Stretches the histogram so that the given fractions map to 0 and 1.
void stretchContrast(Gray& g, float lowFrac = 0.005f, float highFrac = 0.995f);

// Contrast limited adaptive histogram equalisation, blended with the input by `amount`.
void localContrast(Gray& g, int tiles, float clipLimit, float amount);

// Value at quantile q (0..1) of the given samples (the vector is reordered).
float quantile(std::vector<float>& values, float q);

// Rotates/flips according to an EXIF orientation value (1..8).
Rgba applyExifOrientation(const Rgba& img, int orientation);

}  // namespace dz
