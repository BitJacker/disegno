// Saves what the screen shows in a rectangle as a BMP, with the same capture code the app
// uses for calibration (wu::captureScreen).
//
//   capturetest.exe out.bmp x y width height
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/win/winutil.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 6) return 2;
    RECT r{_wtoi(argv[2]), _wtoi(argv[3]), 0, 0};
    r.right = r.left + _wtoi(argv[4]);
    r.bottom = r.top + _wtoi(argv[5]);
    const std::vector<uint32_t> px = wu::captureScreen(r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (px.size() != size_t(w) * size_t(h)) return 1;
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof bi;
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi;
    fh.bfSize = DWORD(fh.bfOffBits + px.size() * 4);
    FILE* f = _wfopen(argv[1], L"wb");
    if (!f) return 1;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&bi, sizeof bi, 1, f);
    fwrite(px.data(), 4, px.size(), f);
    fclose(f);
    return 0;
}
