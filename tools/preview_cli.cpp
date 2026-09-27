// Command line harness for the drawing pipeline (runs on any OS).
// Renders what the app would draw into a PNG and prints statistics.
//
//   preview_cli input.jpg output.png [--area 800x600] [--style 0-4] [--detail 1-10]
//               [--shading 0-10] [--brush px] [--invert] [--stretch] [--upto N]
//               [--timing fast|normal|web|game|gameslow|slow] [--limit seconds]
//               [--frame ms] [--up ms] [--sim fps[,jitter[,hitch[,frames]]]] [--seed n]
//               [--lag buttonFrames,posFrames] [--nosync] [--nopump]
// With --sim the output shows what a game reading the mouse once per frame would draw,
// and the differences from the intended drawing are printed.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/core/pipeline.h"
#include "../src/core/render.h"
#include "../src/core/simulate.h"

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
    std::string timingName = "game";
    double limit = 0;
    float frameMs = -1, upMs = -1;
    dz::FrameModel fm;
    bool sim = false, nosync = false;
    float simT[4] = {-1, 0, 0, 0};
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
        else if (a == "--timing") timingName = next();
        else if (a == "--limit") limit = std::atof(next());
        else if (a == "--frame") frameMs = float(std::atof(next()));
        else if (a == "--up") upMs = float(std::atof(next()));
        else if (a == "--seed") fm.seed = uint32_t(std::atoll(next()));
        else if (a == "--simt") std::sscanf(next(), "%f,%f,%f,%f", &simT[0], &simT[1], &simT[2], &simT[3]);
        else if (a == "--sim") {
            sim = true;
            std::sscanf(next(), "%lf,%lf,%lf,%d", &fm.fps, &fm.jitter, &fm.hitch, &fm.hitchFrames);
        }
        else if (a == "--nosync") nosync = true;
        else if (a == "--nopump") fm.pumps = false;
        else if (a == "--lag") std::sscanf(next(), "%lf,%lf", &fm.buttonLag, &fm.posLag);
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

    const dz::Timing fast = dz::kTimingFast, normal = dz::kTimingNormal, game = dz::kTimingGame;
    dz::Timing timing = timingName == "fast"     ? fast
                      : timingName == "normal"   ? normal
                      : timingName == "web"      ? dz::kTimingWeb
                      : timingName == "gameslow" ? dz::kTimingGameSlow
                      : timingName == "slow"     ? dz::kTimingSlow
                                                 : game;
    if (frameMs >= 0) timing.moveDelayMs = timing.downDelayMs = frameMs;
    if (upMs >= 0) timing.upDelayMs = upMs;
    if (nosync) timing.sync = false;
    auto t0 = std::chrono::steady_clock::now();
    dz::Drawing d;
    dz::buildDrawingFor(gray, float(areaW), float(areaH), p, timing, limit, d);
    auto t1 = std::chrono::steady_clock::now();

    dz::Gray canvas(areaW, areaH, 1.f);
    dz::renderStrokes(canvas, d.strokes, 1.f, 0.f, 0.f, p.brush, 0, upto);
    auto t2 = std::chrono::steady_clock::now();
    if (sim) {
        dz::Timing st = timing;
        if (simT[0] >= 0) st = dz::Timing{simT[0], simT[1], simT[2], simT[3], false};
        const std::vector<dz::Stroke> seen = dz::simulateFrames(d.strokes, st, fm);
        dz::Gray game(areaW, areaH, 1.f);
        dz::renderStrokes(game, seen, 1.f, 0.f, 0.f, p.brush);
        const dz::InkDiff diff = dz::compareInk(canvas, game, 1);
        std::printf("  game %.0f fps (jitter %.2f, hitch %.2f x%d%s): %zu presses seen of %zu, missing %.1f%%, "
                    "extra %.1f%%, stray lines %d, %.0fs\n", fm.fps, fm.jitter, fm.hitch, fm.hitchFrames,
                    st.sync && fm.pumps ? ", synced" : "", seen.size(), d.strokes.size(), diff.missing * 100,
                    diff.extra * 100, dz::countStrayLines(canvas, seen), dz::estimateSeconds(d.strokes, st));
        canvas = game;
    }

    std::vector<unsigned char> out(size_t(areaW) * areaH);
    for (size_t i = 0; i < out.size(); ++i) out[i] = (unsigned char)(canvas.p[i] * 255.f + 0.5f);
    stbi_write_png(argv[2], areaW, areaH, 1, out.data(), areaW);

    size_t layers[8] = {0};
    for (const auto& s : d.strokes) layers[std::min(s.layer, 7)]++;
    std::printf("%s: %zu strokes (outline %zu, shade %zu/%zu/%zu/%zu), %zu points, ink %.0f px\n", argv[1],
                d.strokes.size(), layers[0], layers[1], layers[2], layers[3], layers[4], dz::totalPoints(d.strokes),
                dz::totalLength(d.strokes));
    std::printf("  build %.0f ms, render %.0f ms | %s: %.0fs (coarse %.2f%s) | fast %.0fs normal %.0fs game %.0fs\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count(),
                std::chrono::duration<double, std::milli>(t2 - t1).count(), timingName.c_str(),
                dz::estimateSeconds(d.strokes, timing), d.coarse, d.trimmed ? ", trimmed" : "",
                dz::estimateSeconds(d.strokes, fast), dz::estimateSeconds(d.strokes, normal),
                dz::estimateSeconds(d.strokes, game));
    return 0;
}
