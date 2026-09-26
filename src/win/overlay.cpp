#include "overlay.h"

#include "hotkeys.h"

#include <algorithm>
#include <vector>

namespace overlay {

namespace {

// The on-screen guides are made of small top-most windows (crosshair lines, frame edges,
// speech bubble...). They never take focus and let every click through. Small windows
// are cheap to move and do not disturb full-screen games the way a screen-sized layered
// window can.

const COLORREF kAccent = RGB(99, 102, 241);
const COLORREF kBubble = RGB(24, 24, 32);
const COLORREF kWhite = RGB(255, 255, 255);
const COLORREF kDark = RGB(20, 20, 24);
const COLORREF kMuted = RGB(170, 174, 196);
const COLORREF kAmber = RGB(245, 158, 11);
const wchar_t* kPieceClass = L"DisegnoGuide";
const wchar_t* kCtlClass = L"DisegnoGuideCtl";
const wchar_t* kStatusClass = L"DisegnoStatus";

enum class Mode { None, Corner1, Corner2, Flash, Countdown };
enum class Kind { LineH, LineV, Edge, Marker, Label, Bubble };

struct Piece {
    Kind kind = Kind::Edge;
    HWND hwnd = nullptr;
    RECT rect{};
    bool shown = false;
};

struct State {
    HINSTANCE inst = nullptr;
    HWND ctl = nullptr;     // message-only window that owns the timer
    HWND notify = nullptr;
    Mode mode = Mode::None;
    POINT cursor{-100000, -100000};
    POINT p1{};
    RECT area{};
    ULONGLONG deadline = 0;
    ULONGLONG lastRaise = 0;
    int seconds = 5;
    int number = -1;
    bool postArea = false;
    bool tooSmall = false;
    bool escArmed = false;
    UINT dpi = 96;
    HFONT fBig = nullptr, fText = nullptr, fSmall = nullptr;
    std::vector<Piece> pieces;  // lineH, lineV, 4 edges, marker, label, bubble
    std::wstring labelText;
} g;

enum PieceIndex { kLineH = 0, kLineV, kEdgeTop, kEdgeBottom, kEdgeLeft, kEdgeRight, kMarker, kLabel, kBubblePiece };

struct StatusState {
    HWND hwnd = nullptr;
    std::wstring title, line;
    double fraction = 0;
    bool paused = false;
    UINT dpi = 96;
    HFONT fTitle = nullptr, fText = nullptr;
} st;

int S(int v) { return MulDiv(v, int(g.dpi), 96); }
int SS(int v) { return MulDiv(v, int(st.dpi), 96); }

RECT normalized(POINT a, POINT b) {
    return RECT{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
}

RECT monitorRect(POINT p, bool work) {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    return work ? mi.rcWork : mi.rcMonitor;
}

RECT virtualScreen() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return RECT{x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void drawText(HDC dc, const std::wstring& text, RECT r, HFONT f, COLORREF c, UINT flags) {
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), int(text.size()), &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

void makeFonts(UINT dpi) {
    if (g.fBig && g.dpi == dpi) return;
    for (HFONT f : {g.fBig, g.fText, g.fSmall})
        if (f) DeleteObject(f);
    g.dpi = dpi;
    g.fBig = wu::makeFont(dpi, 30, FW_BOLD);
    g.fText = wu::makeFont(dpi, 11, FW_SEMIBOLD);
    g.fSmall = wu::makeFont(dpi, 9, FW_NORMAL);
}

// ---- piece geometry (screen coordinates) --------------------------------------

SIZE bubbleSize() { return SIZE{S(330), S(112)}; }

RECT bubbleRect() {
    const SIZE sz = bubbleSize();
    const int gap = S(26);
    if (g.mode == Mode::Countdown) {
        POINT c{(g.area.left + g.area.right) / 2, (g.area.top + g.area.bottom) / 2};
        RECT mon = monitorRect(c, true);
        int x = std::clamp(int(c.x - sz.cx / 2), int(mon.left + S(8)), int(mon.right - sz.cx - S(8)));
        int y = g.area.top - gap - sz.cy;
        if (y < mon.top + S(8)) y = g.area.bottom + gap;
        if (y + sz.cy > mon.bottom - S(8)) y = c.y - sz.cy / 2;
        return RECT{x, y, x + sz.cx, y + sz.cy};
    }
    RECT mon = monitorRect(g.cursor, false);
    // Corner 1: keep the bubble up-left (outside the future area); corner 2: down-right.
    const bool upLeft = g.mode == Mode::Corner1;
    int x = upLeft ? g.cursor.x - gap - sz.cx : g.cursor.x + gap;
    int y = upLeft ? g.cursor.y - gap - sz.cy : g.cursor.y + gap;
    if (x < mon.left) x = g.cursor.x + gap;
    if (x + sz.cx > mon.right) x = g.cursor.x - gap - sz.cx;
    if (y < mon.top) y = g.cursor.y + gap;
    if (y + sz.cy > mon.bottom) y = g.cursor.y - gap - sz.cy;
    return RECT{x, y, x + sz.cx, y + sz.cy};
}

RECT selectionRect() {
    if (g.mode == Mode::Corner2) return normalized(g.p1, g.cursor);
    if (g.mode == Mode::Flash || g.mode == Mode::Countdown) return g.area;
    return RECT{0, 0, 0, 0};
}

RECT labelRect(const RECT& sel) {
    const int w = S(124), h = S(26);
    RECT mon = monitorRect(POINT{sel.right, sel.bottom}, false);
    int x = sel.right - w, y = sel.bottom + S(8);
    if (y + h > mon.bottom) y = sel.bottom - h - S(8);
    x = std::max(int(mon.left), x);
    return RECT{x, y, x + w, y + h};
}

// ---- piece painting -----------------------------------------------------------

void paintBubble(HDC dc, RECT b) {
    fill(dc, b, kBubble);
    HBRUSH nb = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, 1, kAccent);
    HGDIOBJ ob = SelectObject(dc, nb), op = SelectObject(dc, pen);
    RoundRect(dc, b.left, b.top, b.right, b.bottom, S(18), S(18));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);

    std::wstring caption, title, hint;
    switch (g.mode) {
        case Mode::Corner1:
            caption = L"PASSO 1 DI 2";
            title = L"Porta il mouse nell'angolo\nIN ALTO A SINISTRA";
            hint = L"Resta fermo lì · ESC = annulla";
            break;
        case Mode::Corner2:
            caption = L"PASSO 2 DI 2";
            title = L"Ora vai nell'angolo\nIN BASSO A DESTRA";
            hint = g.tooSmall ? L"Area troppo piccola, riprova · ESC = annulla" : L"Resta fermo lì · ESC = annulla";
            break;
        default:
            caption = L"PRONTO A DISEGNARE";
            title = L"Il disegno parte tra";
            hint = L"Poi non toccare il mouse · ESC = ferma";
            break;
    }
    const int pad = S(16), circle = S(64);
    RECT num{b.right - pad - circle, b.top + (b.bottom - b.top - circle) / 2, b.right - pad, 0};
    num.bottom = num.top + circle;
    HBRUSH ab = CreateSolidBrush(kAccent);
    HGDIOBJ ob2 = SelectObject(dc, ab), op2 = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, num.left, num.top, num.right + 1, num.bottom + 1);
    SelectObject(dc, ob2);
    SelectObject(dc, op2);
    DeleteObject(ab);
    drawText(dc, std::to_wstring(std::max(0, g.number)), num, g.fBig, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT text{b.left + pad, b.top + S(12), num.left - S(10), b.bottom - S(10)};
    RECT cap = text;
    cap.bottom = cap.top + S(18);
    drawText(dc, caption, cap, g.fSmall, kMuted, DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT tt = text;
    tt.top = cap.bottom + S(2);
    tt.bottom = tt.top + S(44);
    drawText(dc, title, tt, g.fText, kWhite, DT_LEFT | DT_TOP);
    RECT hh = text;
    hh.top = b.bottom - S(28);
    drawText(dc, hint, hh, g.fSmall, g.tooSmall && g.mode == Mode::Corner2 ? kAmber : kMuted,
             DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void paintPiece(const Piece& p, HDC dc, RECT rc) {
    switch (p.kind) {
        case Kind::LineH: {
            fill(dc, rc, kDark);
            RECT mid{rc.left, rc.top + 1, rc.right, rc.top + 2};
            fill(dc, mid, kWhite);
            break;
        }
        case Kind::LineV: {
            fill(dc, rc, kDark);
            RECT mid{rc.left + 1, rc.top, rc.left + 2, rc.bottom};
            fill(dc, mid, kWhite);
            break;
        }
        case Kind::Edge:
            fill(dc, rc, kAccent);
            break;
        case Kind::Marker: {
            fill(dc, rc, kWhite);
            RECT in = rc;
            InflateRect(&in, -S(2), -S(2));
            fill(dc, in, kAccent);
            break;
        }
        case Kind::Label:
            fill(dc, rc, kBubble);
            drawText(dc, g.labelText, rc, g.fSmall, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        case Kind::Bubble:
            paintBubble(dc, rc);
            break;
    }
}

LRESULT CALLBACK pieceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
            HGDIOBJ old = SelectObject(mem, bmp);
            for (const Piece& p : g.pieces)
                if (p.hwnd == hwnd) paintPiece(p, mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND createGuideWindow(const wchar_t* cls, BYTE alpha) {
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             cls, L"Disegno", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g.inst, nullptr);
    if (h) SetLayeredWindowAttributes(h, 0, alpha, LWA_ALPHA);
    return h;
}

void placePiece(Piece& p, RECT r, bool show, bool repaint) {
    if (!p.hwnd) return;
    if (!show || r.right <= r.left || r.bottom <= r.top) {
        if (p.shown) ShowWindow(p.hwnd, SW_HIDE);
        p.shown = false;
        return;
    }
    const bool moved = !EqualRect(&r, &p.rect);
    const bool resized = (r.right - r.left) != (p.rect.right - p.rect.left) || (r.bottom - r.top) != (p.rect.bottom - p.rect.top);
    if (moved || !p.shown) {
        SetWindowPos(p.hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOACTIVATE | (p.shown ? 0 : SWP_SHOWWINDOW));
        if (resized && (p.kind == Kind::Bubble || p.kind == Kind::Label)) {
            const int rad = p.kind == Kind::Bubble ? S(18) : S(12);
            SetWindowRgn(p.hwnd, CreateRoundRectRgn(0, 0, r.right - r.left + 1, r.bottom - r.top + 1, rad, rad), TRUE);
        }
        p.rect = r;
        p.shown = true;
    }
    if (repaint || resized) InvalidateRect(p.hwnd, nullptr, FALSE);
}

// Positions every guide window for the current state.
void update(bool repaint) {
    if (g.pieces.empty()) return;
    const bool corners = g.mode == Mode::Corner1 || g.mode == Mode::Corner2;
    const RECT vs = virtualScreen();
    placePiece(g.pieces[kLineH], RECT{vs.left, g.cursor.y - 1, vs.right, g.cursor.y + 2}, corners, false);
    placePiece(g.pieces[kLineV], RECT{g.cursor.x - 1, vs.top, g.cursor.x + 2, vs.bottom}, corners, false);

    RECT sel = selectionRect();
    const bool hasSel = (sel.right > sel.left || sel.bottom > sel.top) && g.mode != Mode::Corner1;
    const int t = g.mode == Mode::Flash ? S(4) : S(3);
    placePiece(g.pieces[kEdgeTop], RECT{sel.left - t, sel.top - t, sel.right + t, sel.top}, hasSel, false);
    placePiece(g.pieces[kEdgeBottom], RECT{sel.left - t, sel.bottom, sel.right + t, sel.bottom + t}, hasSel, false);
    placePiece(g.pieces[kEdgeLeft], RECT{sel.left - t, sel.top, sel.left, sel.bottom}, hasSel, false);
    placePiece(g.pieces[kEdgeRight], RECT{sel.right, sel.top, sel.right + t, sel.bottom}, hasSel, false);

    const int m = S(6);
    placePiece(g.pieces[kMarker], RECT{g.p1.x - m, g.p1.y - m, g.p1.x + m + 1, g.p1.y + m + 1},
               g.mode == Mode::Corner2, false);

    const bool showLabel = hasSel && g.mode != Mode::Countdown;
    std::wstring label = std::to_wstring(sel.right - sel.left) + L" × " + std::to_wstring(sel.bottom - sel.top) + L" px";
    const bool labelChanged = label != g.labelText;
    g.labelText = label;
    placePiece(g.pieces[kLabel], showLabel ? labelRect(sel) : RECT{}, showLabel, repaint || labelChanged);

    const bool showBubble = corners || g.mode == Mode::Countdown;
    placePiece(g.pieces[kBubblePiece], showBubble ? bubbleRect() : RECT{}, showBubble, repaint);

    // Other top-most windows (games, taskbar) may cover us: re-assert the order now and then.
    const ULONGLONG now = GetTickCount64();
    if (now - g.lastRaise > 1000) {
        g.lastRaise = now;
        for (const Piece& p : g.pieces)
            if (p.shown)
                SetWindowPos(p.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// ---- state machine ------------------------------------------------------------

void closeGuides() {
    if (g.ctl) KillTimer(g.ctl, 1);
    for (Piece& p : g.pieces)
        if (p.hwnd) DestroyWindow(p.hwnd);
    g.pieces.clear();
    g.mode = Mode::None;
}

void finish(bool ok) {
    const Mode mode = g.mode;
    const bool postArea = g.postArea;
    const RECT area = g.area;
    const HWND notify = g.notify;
    closeGuides();
    if (mode == Mode::Countdown) {
        PostMessageW(notify, WM_APP_COUNTDOWN_DONE, ok ? 1 : 0, 0);
    } else if (mode == Mode::Corner1 || mode == Mode::Corner2) {
        PostMessageW(notify, WM_APP_AREA_PICKED, 0, 0);
    } else if (mode == Mode::Flash && postArea) {
        PostMessageW(notify, WM_APP_AREA_PICKED, ok ? 1 : 0, ok ? reinterpret_cast<LPARAM>(new RECT(area)) : 0);
    }
}

int secondsLeft(ULONGLONG now) { return now >= g.deadline ? 0 : int((g.deadline - now + 999) / 1000); }

void tick() {
    if (g.mode == Mode::None) return;
    const bool escHook = hotkeys::takeStop();
    const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (!esc) g.escArmed = true;
    if ((escHook || (esc && g.escArmed && !hotkeys::active())) && g.mode != Mode::Flash) {
        finish(false);
        return;
    }
    POINT c;
    GetCursorPos(&c);
    const ULONGLONG now = GetTickCount64();
    bool repaint = false;
    if (now >= g.deadline) {
        switch (g.mode) {
            case Mode::Corner1:
                g.p1 = c;
                g.mode = Mode::Corner2;
                g.deadline = now + ULONGLONG(g.seconds) * 1000;
                MessageBeep(MB_OK);
                repaint = true;
                break;
            case Mode::Corner2: {
                RECT r = normalized(g.p1, c);
                if (r.right - r.left < 8 || r.bottom - r.top < 8) {
                    g.tooSmall = true;
                    g.deadline = now + ULONGLONG(g.seconds) * 1000;
                    MessageBeep(MB_ICONHAND);
                } else {
                    g.area = r;
                    g.mode = Mode::Flash;
                    g.deadline = now + 1100;
                    g.postArea = true;
                    MessageBeep(MB_OK);
                }
                repaint = true;
                break;
            }
            case Mode::Flash:
            case Mode::Countdown:
                finish(true);
                return;
            default:
                return;
        }
    }
    const int n = secondsLeft(now);
    if (n != g.number) repaint = true;
    if (repaint || c.x != g.cursor.x || c.y != g.cursor.y) {
        g.cursor = c;
        g.number = n;
        const POINT ref = g.mode == Mode::Countdown || g.mode == Mode::Flash
                              ? POINT{(g.area.left + g.area.right) / 2, (g.area.top + g.area.bottom) / 2}
                              : c;
        const UINT dpi = wu::dpiForPoint(ref);
        if (dpi != g.dpi) {
            makeFonts(dpi);
            for (Piece& p : g.pieces) p.rect = RECT{};  // force resize/region update
            repaint = true;
        }
        update(repaint);
    }
}

LRESULT CALLBACK ctlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == 1) {
        tick();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void open(Mode mode) {
    closeGuides();
    g.mode = mode;
    g.number = -1;
    g.tooSmall = false;
    g.postArea = false;
    g.lastRaise = 0;
    g.escArmed = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) == 0;
    GetCursorPos(&g.cursor);
    makeFonts(wu::dpiForPoint(mode == Mode::Countdown || mode == Mode::Flash
                                  ? POINT{(g.area.left + g.area.right) / 2, (g.area.top + g.area.bottom) / 2}
                                  : g.cursor));
    const Kind kinds[] = {Kind::LineH, Kind::LineV, Kind::Edge,  Kind::Edge,
                          Kind::Edge,  Kind::Edge,  Kind::Marker, Kind::Label, Kind::Bubble};
    for (Kind k : kinds) {
        Piece p;
        p.kind = k;
        p.hwnd = createGuideWindow(kPieceClass, k == Kind::Bubble || k == Kind::Label ? 245 : 255);
        g.pieces.push_back(p);
    }
    if (!g.ctl)
        g.ctl = CreateWindowExW(0, kCtlClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g.inst, nullptr);
    SetTimer(g.ctl, 1, 15, nullptr);
    tick();
    update(true);
}

// ---- status box -----------------------------------------------------------------

LRESULT CALLBACK statusProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == 2) hideStatus(0);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            fill(mem, rc, kBubble);
            const int pad = SS(14);
            RECT t{pad, SS(10), rc.right - pad, SS(34)};
            HGDIOBJ of = SelectObject(mem, st.fTitle);
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, kWhite);
            DrawTextW(mem, st.title.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT bar{pad, SS(40), rc.right - pad, SS(48)};
            fill(mem, bar, RGB(58, 58, 74));
            RECT done = bar;
            done.right = bar.left + LONG(double(bar.right - bar.left) * std::clamp(st.fraction, 0.0, 1.0));
            fill(mem, done, st.paused ? kAmber : kAccent);
            SelectObject(mem, st.fText);
            SetTextColor(mem, kMuted);
            RECT l{pad, SS(54), rc.right - pad, rc.bottom - SS(6)};
            DrawTextW(mem, st.line.c_str(), -1, &l, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(mem, of);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool registerClasses(HINSTANCE inst) {
    g.inst = inst;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = pieceProc;
    wc.lpszClassName = kPieceClass;
    if (!RegisterClassExW(&wc)) return false;
    wc.lpfnWndProc = ctlProc;
    wc.lpszClassName = kCtlClass;
    if (!RegisterClassExW(&wc)) return false;
    wc.lpfnWndProc = statusProc;
    wc.lpszClassName = kStatusClass;
    return RegisterClassExW(&wc) != 0;
}

void pickArea(HWND notify, int seconds) {
    g.notify = notify;
    g.seconds = std::clamp(seconds, 1, 60);
    g.deadline = GetTickCount64() + ULONGLONG(g.seconds) * 1000;
    open(Mode::Corner1);
}

void flashArea(const RECT& area, int ms) {
    if (busy()) return;
    g.notify = nullptr;
    g.area = area;
    g.deadline = GetTickCount64() + ULONGLONG(std::max(100, ms));
    open(Mode::Flash);
}

void countdown(HWND notify, const RECT& area, int seconds) {
    g.notify = notify;
    g.area = area;
    g.seconds = std::clamp(seconds, 0, 60);
    g.deadline = GetTickCount64() + ULONGLONG(g.seconds) * 1000;
    open(Mode::Countdown);
}

void cancel() {
    if (g.mode != Mode::None) finish(false);
}

bool busy() { return g.mode != Mode::None && g.mode != Mode::Flash; }

void showStatus(const RECT& avoid) {
    hideStatus(0);
    POINT c{(avoid.left + avoid.right) / 2, (avoid.top + avoid.bottom) / 2};
    st.dpi = wu::dpiForPoint(c);
    if (st.fTitle) DeleteObject(st.fTitle);
    if (st.fText) DeleteObject(st.fText);
    st.fTitle = wu::makeFont(st.dpi, 11, FW_SEMIBOLD);
    st.fText = wu::makeFont(st.dpi, 9);
    const int w = SS(380), h = SS(78), m = SS(16);
    RECT mon = monitorRect(c, true);
    const POINT candidates[4] = {{mon.right - w - m, mon.bottom - h - m},
                                 {mon.left + m, mon.bottom - h - m},
                                 {mon.right - w - m, mon.top + m},
                                 {mon.left + m, mon.top + m}};
    POINT pos = candidates[0];
    for (const POINT& p : candidates) {
        RECT r{p.x, p.y, p.x + w, p.y + h}, tmp;
        if (!IntersectRect(&tmp, &r, &avoid)) {
            pos = p;
            break;
        }
    }
    st.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kStatusClass, L"Disegno", WS_POPUP, pos.x, pos.y, w, h, nullptr, nullptr, g.inst,
                              nullptr);
    if (!st.hwnd) return;
    SetLayeredWindowAttributes(st.hwnd, 0, 238, LWA_ALPHA);
    SetWindowRgn(st.hwnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, SS(16), SS(16)), FALSE);
    ShowWindow(st.hwnd, SW_SHOWNOACTIVATE);
}

void setStatus(const std::wstring& title, const std::wstring& line, double fraction, bool paused) {
    st.title = title;
    st.line = line;
    st.fraction = fraction;
    st.paused = paused;
    if (st.hwnd) InvalidateRect(st.hwnd, nullptr, FALSE);
}

void hideStatus(int afterMs) {
    if (!st.hwnd) return;
    if (afterMs > 0) {
        SetTimer(st.hwnd, 2, UINT(afterMs), nullptr);
        return;
    }
    KillTimer(st.hwnd, 2);
    DestroyWindow(st.hwnd);
    st.hwnd = nullptr;
}

}  // namespace overlay
