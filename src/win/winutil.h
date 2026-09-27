// Small Win32 helpers shared by the Windows front end.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Private window messages.
enum : UINT {
    WM_APP_PROCESSED = WM_APP + 1,   // wParam = generation, lParam = dz::Drawing* (may be null)
    WM_APP_AREA_PICKED,              // wParam = 1 ok / 0 cancelled, lParam = RECT* (heap, ok only)
    WM_APP_COUNTDOWN_DONE,           // wParam = 1 ok / 0 cancelled
    WM_APP_DRAW_PROGRESS,            // wParam = strokes done, lParam = total
    WM_APP_DRAW_PAUSED,              // wParam = 1 paused / 0 resumed
    WM_APP_DRAW_DONE,                // wParam = DrawResult, lParam = strokes done
};

// Binary blobs (encoded images, thumbnails) travel as std::string.
using Bytes = std::string;

namespace wu {

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

UINT systemDpi();
UINT dpiForWindow(HWND hwnd);
UINT dpiForPoint(POINT pt);
inline int scale(int v, UINT dpi) { return MulDiv(v, int(dpi), 96); }
bool adjustWindowRectForDpi(RECT* rc, DWORD style, DWORD exStyle, UINT dpi);

HFONT makeFont(UINT dpi, int pointSize, int weight = FW_NORMAL, const wchar_t* face = L"Segoe UI");

std::wstring exeDir();
// Data folder: next to the exe when "disegno-portable.txt" exists there,
// otherwise %LOCALAPPDATA%\Disegno. Created on demand.
std::wstring dataDir();

std::wstring formatDuration(double seconds);   // "45 s", "3 min 20 s", "1 h 05 min"
std::wstring formatInt(long long n);           // "12.345"
std::wstring fileStem(const std::wstring& path);
std::wstring nowStamp();                       // "26/09/2026 22:10"

bool readFile(const std::wstring& path, std::string& out);
bool writeFile(const std::wstring& path, const void* data, size_t size);

// What the screen shows inside `r` (screen pixels), row by row from the top, 32 bits per
// pixel. Empty on failure.
std::vector<uint32_t> captureScreen(const RECT& r);

}  // namespace wu
