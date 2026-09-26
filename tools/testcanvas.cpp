// Minimal "Paint"-like canvas for automated tests: draws 1px black lines while the left
// button is held, and saves the canvas as a BMP once a second (and on exit).
//
//   testcanvas.exe out.bmp [x y width height [poll|events] [window title]]
// Writes the client area position to out.bmp.txt as "left top width height".
// With "poll" it behaves like a game (e.g. Roblox): instead of handling every mouse
// message it looks at the cursor and the button once per frame (16 ms) and joins the
// positions it sees while the button is down.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

HDC g_mem = nullptr;
HBITMAP g_bmp = nullptr;
int g_w = 600, g_h = 450;
bool g_down = false, g_dirty = false;
POINT g_last{};
std::wstring g_out = L"canvas.bmp";
long g_moves = 0, g_presses = 0;
bool g_poll = false;
bool g_pollDown = false;

void save() {
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof bi;
    bi.biWidth = g_w;
    bi.biHeight = g_h;  // bottom-up
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    const int stride = ((g_w * 3 + 3) / 4) * 4;
    std::string px(size_t(stride) * g_h, '\0');
    GetDIBits(g_mem, g_bmp, 0, UINT(g_h), px.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi;
    fh.bfSize = DWORD(fh.bfOffBits + px.size());
    std::wstring tmp = g_out + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&bi, sizeof bi, 1, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
    MoveFileExW(tmp.c_str(), g_out.c_str(), MOVEFILE_REPLACE_EXISTING);
    std::wstring stats = g_out + L".stats";
    if (FILE* s = _wfopen(stats.c_str(), L"w")) {
        fprintf(s, "presses %ld moves %ld\n", g_presses, g_moves);
        fclose(s);
    }
    g_dirty = false;
}

void lineTo(POINT p) {
    MoveToEx(g_mem, g_last.x, g_last.y, nullptr);
    LineTo(g_mem, p.x, p.y);
    SetPixel(g_mem, p.x, p.y, RGB(0, 0, 0));
    g_last = p;
    g_dirty = true;
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HDC dc = GetDC(hwnd);
            g_mem = CreateCompatibleDC(dc);
            g_bmp = CreateCompatibleBitmap(dc, g_w, g_h);
            ReleaseDC(hwnd, dc);
            SelectObject(g_mem, g_bmp);
            RECT r{0, 0, g_w, g_h};
            FillRect(g_mem, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            SelectObject(g_mem, GetStockObject(BLACK_PEN));
            SetTimer(hwnd, 1, 1000, nullptr);
            if (g_poll) SetTimer(hwnd, 2, 16, nullptr);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (g_poll) return 0;
            SetCapture(hwnd);
            g_down = true;
            ++g_presses;
            g_last = POINT{short(LOWORD(lp)), short(HIWORD(lp))};
            SetPixel(g_mem, g_last.x, g_last.y, RGB(0, 0, 0));
            g_dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (!g_poll && g_down && (wp & MK_LBUTTON)) {
                ++g_moves;
                lineTo(POINT{short(LOWORD(lp)), short(HIWORD(lp))});
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (g_poll) return 0;
            if (g_down) lineTo(POINT{short(LOWORD(lp)), short(HIWORD(lp))});
            g_down = false;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wp == 2) {
                // One "game frame": sample the cursor and the button state.
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                const bool inside = p.x >= 0 && p.y >= 0 && p.x < g_w && p.y < g_h;
                if (down && !g_pollDown && inside) {
                    ++g_presses;
                    g_last = p;
                    SetPixel(g_mem, p.x, p.y, RGB(0, 0, 0));
                    g_dirty = true;
                    g_pollDown = true;
                } else if (down && g_pollDown) {
                    ++g_moves;
                    lineTo(p);
                } else if (!down) {
                    g_pollDown = false;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_dirty) save();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            BitBlt(dc, 0, 0, g_w, g_h, g_mem, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            save();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int x = 100, y = 100;
    if (argc > 1) g_out = argv[1];
    if (argc > 5) {
        x = _wtoi(argv[2]);
        y = _wtoi(argv[3]);
        g_w = _wtoi(argv[4]);
        g_h = _wtoi(argv[5]);
    }
    if (argc > 6 && lstrcmpiW(argv[6], L"poll") == 0) g_poll = true;
    const wchar_t* title = argc > 7 ? argv[7] : L"TestCanvas";
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = L"TestCanvas";
    RegisterClassW(&wc);
    RECT r{0, 0, g_w, g_h};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowW(L"TestCanvas", title, style, x, y, r.right - r.left, r.bottom - r.top, nullptr,
                              nullptr, inst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    std::wstring info = g_out + L".txt";
    if (FILE* f = _wfopen(info.c_str(), L"w")) {
        fprintf(f, "%ld %ld %d %d\n", origin.x, origin.y, g_w, g_h);
        fclose(f);
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
