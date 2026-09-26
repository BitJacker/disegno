// Command line harness for the drawing pipeline (runs on any OS).
// Renders what the app would draw into a PNG and prints statistics.
//
//   preview_cli input.jpg output.png [--area 800x600] [--style 0-3] [--detail 1-10]
//               [--shading 0-10] [--brush px] [--invert] [--stretch] [--upto N]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/core/pipeline.h"
#include "../src/core/render.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s input output.png [options]\n", argv[0]);
        return 2;
    }
    dz::Params p;
    int areaW = 800, areaH = 600;
    size_t upto = size_t(-1);
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
        if (a == "--area") std::sscanf(next(), "%dx%d", &areaW, &areaH);
        else if (a == "--style") p.style = dz::Style(std::atoi(next()));
        else if (a == "--detail") p.detail = std::atoi(next());
        else if (a == "--shading") p.shading = std::atoi(next());
        else if (a == "--brush") p.brush = float(std::atof(next()));
        else if (a == "--invert") p.invert = true;
        else if (a == "--stretch") p.stretch = true;
        else if (a == "--upto") upto = size_t(std::atoll(next()));
    }

    int w, h, n;
    unsigned char* data = stbi_load(argv[1], &w, &h, &n, 4);
    if (!data) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    dz::Rgba rgba(w, h);
    std::memcpy(rgba.p.data(), data, size_t(w) * h * 4);
    stbi_image_free(data);
    dz::Gray gray = dz::toGray(rgba);

    auto t0 = std::chrono::steady_clock::now();
    dz::Drawing d;
    dz::buildDrawing(gray, float(areaW), float(areaH), p, d);
    auto t1 = std::chrono::steady_clock::now();

    dz::Gray canvas(areaW, areaH, 1.f);
    dz::renderStrokes(canvas, d.strokes, 1.f, 0.f, 0.f, p.brush, 0, upto);
    auto t2 = std::chrono::steady_clock::now();

    std::vector<unsigned char> out(size_t(areaW) * areaH);
    for (size_t i = 0; i < out.size(); ++i) out[i] = (unsigned char)(canvas.p[i] * 255.f + 0.5f);
    stbi_write_png(argv[2], areaW, areaH, 1, out.data(), areaW);

    size_t layers[8] = {0};
    for (const auto& s : d.strokes) layers[std::min(s.layer, 7)]++;
    dz::Timing fast{12, 1, 2, 2, false}, normal{6, 2, 8, 8, false}, game{4, 12, 35, 35, true};
    std::printf("%s: %zu strokes (outline %zu, shade %zu/%zu/%zu/%zu), %zu points, ink %.0f px\n", argv[1],
                d.strokes.size(), layers[0], layers[1], layers[2], layers[3], layers[4], dz::totalPoints(d.strokes),
                dz::totalLength(d.strokes));
    std::printf("  build %.0f ms, render %.0f ms | time fast %.0fs normal %.0fs game %.0fs\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count(),
                std::chrono::duration<double, std::milli>(t2 - t1).count(), dz::estimateSeconds(d.strokes, fast),
                dz::estimateSeconds(d.strokes, normal), dz::estimateSeconds(d.strokes, game));
    return 0;
}
