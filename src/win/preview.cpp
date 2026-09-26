#include "preview.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "../core/pipeline.h"
#include "../core/render.h"

namespace preview {

namespace {

const wchar_t* kClass = L"DisegnoPreview";
const COLORREF kBg = RGB(232, 233, 239);
const COLORREF kAccent = RGB(99, 102, 241);
const COLORREF kText = RGB(40, 42, 54);
const COLORREF kMuted = RGB(110, 114, 130);

enum Button { kNone = -1, kBtnDrawing = 0, kBtnPhoto = 1, kBtnAnim = 2 };

struct State {
    Content c;
    bool photoMode = false;
    bool playing = false;
    size_t animDone = 0;
    dz::Gray animCanvas;
    HBITMAP bmp = nullptr;
    uint32_t* bits = nullptr;
    int bmpW = 0, bmpH = 0;
    bool dirty = true;
    UINT dpi = 0;
    HFONT font = nullptr, fontBold = nullptr;
    RECT buttons[3]{};
    int hot = kNone;
    bool tracking = false;
};

State* get(HWND h) { return reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

int S(const State* st, int v) { return MulDiv(v, int(st->dpi ? st->dpi : 96), 96); }

void ensureFonts(HWND hwnd, State* st) {
    UINT dpi = wu::dpiForWindow(hwnd);
    if (st->font && dpi == st->dpi) return;
    if (st->font) DeleteObject(st->font);
    if (st->fontBold) DeleteObject(st->fontBold);
    st->dpi = dpi;
    st->font = wu::makeFont(dpi, 9);
    st->fontBold = wu::makeFont(dpi, 9, FW_SEMIBOLD);
}

void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void roundRect(HDC dc, RECT r, int radius, COLORREF fillC, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fillC);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void text(HDC dc, const wchar_t* s, RECT r, HFONT f, COLORREF c, UINT flags) {
    HGDIOBJ o = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, o);
}

void layoutButtons(State* st, const RECT& rc) {
    const int h = S(st, 28), top = S(st, 10), w = S(st, 96);
    st->buttons[kBtnDrawing] = RECT{S(st, 16), top, S(st, 16) + w, top + h};
    st->buttons[kBtnPhoto] = RECT{S(st, 16) + w, top, S(st, 16) + 2 * w, top + h};
    st->buttons[kBtnAnim] = RECT{rc.right - S(st, 16) - S(st, 118), top, rc.right - S(st, 16), top + h};
}

RECT paperRect(const State* st, const RECT& rc) {
    RECT avail{S(st, 16), S(st, 50), rc.right - S(st, 16), rc.bottom - S(st, 16)};
    float aw = st->c.areaW > 0 ? st->c.areaW : 4.f, ah = st->c.areaH > 0 ? st->c.areaH : 3.f;
    float availW = float(std::max(1L, avail.right - avail.left)), availH = float(std::max(1L, avail.bottom - avail.top));
    float s = std::min(availW / aw, availH / ah);
    int pw = std::max(1, int(aw * s)), ph = std::max(1, int(ah * s));
    int x = avail.left + (int(availW) - pw) / 2, y = avail.top + (int(availH) - ph) / 2;
    return RECT{x, y, x + pw, y + ph};
}

void freeBitmap(State* st) {
    if (st->bmp) DeleteObject(st->bmp);
    st->bmp = nullptr;
    st->bits = nullptr;
    st->bmpW = st->bmpH = 0;
}

bool ensureBitmap(State* st, int w, int h) {
    if (st->bmp && st->bmpW == w && st->bmpH == h) return true;
    freeBitmap(st);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    st->bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!st->bmp) return false;
    st->bits = static_cast<uint32_t*>(bits);
    st->bmpW = w;
    st->bmpH = h;
    return true;
}

void grayToBitmap(State* st, const dz::Gray& g) {
    const size_t n = size_t(st->bmpW) * size_t(st->bmpH);
    for (size_t i = 0; i < n && i < g.p.size(); ++i) {
        uint32_t v = uint32_t(std::clamp(g.p[i], 0.f, 1.f) * 255.f + 0.5f);
        st->bits[i] = (v << 16) | (v << 8) | v;
    }
}

float drawingScale(const State* st) {
    const dz::Drawing* d = st->c.drawing.get();
    return d && d->width > 0 ? float(st->bmpW) / d->width : 1.f;
}

float lineWidth(const State* st) {
    const dz::Drawing* d = st->c.drawing.get();
    return std::max(0.6f, (d ? d->brush : 1.f) * drawingScale(st));
}

void rebuild(State* st, int w, int h) {
    if (!ensureBitmap(st, w, h)) return;
    std::fill(st->bits, st->bits + size_t(w) * size_t(h), 0x00FFFFFFu);
    if (st->photoMode && st->c.photo && !st->c.photo->empty()) {
        const dz::Rgba& ph = *st->c.photo;
        dz::Layout L = dz::fitLayout(ph.w, ph.h, st->c.areaW, st->c.areaH, st->c.stretch);
        float s = float(w) / std::max(1.f, st->c.areaW);
        int dx = int(std::lround(L.ox * s)), dy = int(std::lround(L.oy * s));
        int dw = std::max(1, int(std::lround(L.w * s))), dh = std::max(1, int(std::lround(L.h * s)));
        dz::Rgba img = dz::resize(ph, dw, dh);
        for (int y = 0; y < dh; ++y) {
            int ty = dy + y;
            if (ty < 0 || ty >= h) continue;
            for (int x = 0; x < dw; ++x) {
                int tx = dx + x;
                if (tx < 0 || tx >= w) continue;
                const uint8_t* p = &img.p[(size_t(y) * dw + x) * 4];
                uint32_t a = p[3];
                uint32_t r = (p[0] * a + 255 * (255 - a)) / 255;
                uint32_t g = (p[1] * a + 255 * (255 - a)) / 255;
                uint32_t b = (p[2] * a + 255 * (255 - a)) / 255;
                st->bits[size_t(ty) * w + tx] = (r << 16) | (g << 8) | b;
            }
        }
    } else if (st->c.drawing) {
        if (st->playing) {
            st->animCanvas = dz::Gray(w, h, 1.f);
            dz::renderStrokes(st->animCanvas, st->c.drawing->strokes, drawingScale(st), 0, 0, lineWidth(st), 0,
                              st->animDone);
            grayToBitmap(st, st->animCanvas);
        } else {
            dz::Gray canvas(w, h, 1.f);
            dz::renderStrokes(canvas, st->c.drawing->strokes, drawingScale(st), 0, 0, lineWidth(st));
            grayToBitmap(st, canvas);
        }
    }
    st->dirty = false;
}

void stopAnimation(HWND hwnd, State* st) {
    if (!st->playing) return;
    st->playing = false;
    KillTimer(hwnd, 1);
    st->dirty = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void animate(HWND hwnd, State* st) {
    if (!st->playing || !st->c.drawing || !st->bmp || st->animCanvas.w != st->bmpW) {
        stopAnimation(hwnd, st);
        return;
    }
    const auto& strokes = st->c.drawing->strokes;
    size_t step = std::max<size_t>(1, strokes.size() / 200);
    size_t next = std::min(strokes.size(), st->animDone + step);
    dz::renderStrokes(st->animCanvas, strokes, drawingScale(st), 0, 0, lineWidth(st), st->animDone, next);
    st->animDone = next;
    grayToBitmap(st, st->animCanvas);
    if (st->animDone >= strokes.size()) {
        st->playing = false;
        KillTimer(hwnd, 1);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void paint(HWND hwnd, State* st, HDC dc) {
    ensureFonts(hwnd, st);
    RECT rc;
    GetClientRect(hwnd, &rc);
    layoutButtons(st, rc);
    fill(dc, rc, kBg);

    // Segmented switch: drawing / photo.
    const wchar_t* labels[2] = {L"Disegno", L"Foto"};
    for (int i = 0; i < 2; ++i) {
        bool on = (i == 1) == st->photoMode;
        RECT b = st->buttons[i];
        COLORREF bg = on ? kAccent : (st->hot == i ? RGB(245, 245, 250) : RGB(255, 255, 255));
        roundRect(dc, b, S(st, 10), bg, on ? kAccent : RGB(208, 210, 220));
        text(dc, labels[i], b, on ? st->fontBold : st->font, on ? RGB(255, 255, 255) : kText,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    const bool canAnimate = st->c.drawing && !st->c.drawing->strokes.empty() && !st->photoMode;
    {
        RECT b = st->buttons[kBtnAnim];
        roundRect(dc, b, S(st, 10), st->hot == kBtnAnim && canAnimate ? RGB(245, 245, 250) : RGB(255, 255, 255),
                  RGB(208, 210, 220));
        text(dc, st->playing ? L"■  Ferma" : L"►  Guarda l'ordine", b, st->font,
             canAnimate ? kText : RGB(170, 172, 182), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT paper = paperRect(st, rc);
    const int pw = paper.right - paper.left, ph = paper.bottom - paper.top;
    RECT shadow = paper;
    OffsetRect(&shadow, S(st, 2), S(st, 3));
    fill(dc, shadow, RGB(200, 202, 212));
    const bool hasContent = st->photoMode ? bool(st->c.photo) : bool(st->c.drawing);
    if (hasContent) {
        if (st->dirty || st->bmpW != pw || st->bmpH != ph) rebuild(st, pw, ph);
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, st->bmp);
        BitBlt(dc, paper.left, paper.top, pw, ph, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        if (st->playing && st->animDone > 0) {
            const auto& s = st->c.drawing->strokes[st->animDone - 1];
            if (!s.pts.empty()) {
                float sc = drawingScale(st);
                int x = paper.left + int(s.pts.back().x * sc), y = paper.top + int(s.pts.back().y * sc);
                HBRUSH b = CreateSolidBrush(RGB(239, 68, 68));
                HGDIOBJ ob = SelectObject(dc, b);
                HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc, x - S(st, 5), y - S(st, 5), x + S(st, 6), y + S(st, 6));
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(b);
            }
        }
    } else {
        fill(dc, paper, RGB(255, 255, 255));
        RECT t = paper;
        InflateRect(&t, -S(st, 20), -S(st, 20));
        const wchar_t* msg = st->c.photo || st->c.drawing
                                 ? L"Preparazione dell'anteprima…"
                                 : L"Apri o incolla una foto per iniziare\n\nClicca qui, trascina un'immagine "
                                   L"nella finestra oppure premi Ctrl+V";
        RECT calc = t;
        HGDIOBJ o = SelectObject(dc, st->font);
        DrawTextW(dc, msg, -1, &calc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        SelectObject(dc, o);
        int th = calc.bottom - calc.top;
        t.top = paper.top + (ph - th) / 2;
        t.bottom = t.top + th + 2;
        text(dc, msg, t, st->font, kMuted, DT_CENTER | DT_WORDBREAK);
    }
    if (st->c.busy) {
        RECT pill{paper.right - S(st, 130), paper.top + S(st, 8), paper.right - S(st, 8), paper.top + S(st, 32)};
        roundRect(dc, pill, S(st, 12), RGB(30, 30, 40), RGB(30, 30, 40));
        text(dc, L"Elaborazione…", pill, st->font, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

int hitTest(State* st, POINT p) {
    for (int i = 0; i < 3; ++i)
        if (PtInRect(&st->buttons[i], p)) return i;
    return kNone;
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    State* st = get(hwnd);
    switch (msg) {
        case WM_NCCREATE: {
            auto* s = new State();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            break;
        }
        case WM_NCDESTROY:
            if (st) {
                freeBitmap(st);
                if (st->font) DeleteObject(st->font);
                if (st->fontBold) DeleteObject(st->fontBold);
                delete st;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (st) {
                stopAnimation(hwnd, st);
                st->dirty = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (rc.right > 0 && rc.bottom > 0) {
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                HGDIOBJ old = SelectObject(mem, bmp);
                paint(hwnd, st, mem);
                BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == 1) animate(hwnd, st);
            return 0;
        case WM_MOUSEMOVE: {
            POINT p{short(LOWORD(lp)), short(HIWORD(lp))};
            int h = hitTest(st, p);
            if (h != st->hot) {
                st->hot = h;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (!st->tracking) {
                TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
                st->tracking = TrackMouseEvent(&tme) != FALSE;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            st->tracking = false;
            if (st->hot != kNone) {
                st->hot = kNone;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            if (LOWORD(lp) == HTCLIENT && hitTest(st, p) != kNone) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT p{short(LOWORD(lp)), short(HIWORD(lp))};
            int h = hitTest(st, p);
            if (h == kBtnDrawing || h == kBtnPhoto) {
                showPhoto(hwnd, h == kBtnPhoto);
            } else if (h == kBtnAnim) {
                if (st->playing) {
                    stopAnimation(hwnd, st);
                } else if (st->c.drawing && !st->c.drawing->strokes.empty() && !st->photoMode && st->bmp) {
                    st->playing = true;
                    st->animDone = 0;
                    st->animCanvas = dz::Gray(st->bmpW, st->bmpH, 1.f);
                    SetTimer(hwnd, 1, 33, nullptr);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else if (!st->c.photo) {
                // Empty sheet: behave like the "open image" button.
                SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED),
                             reinterpret_cast<LPARAM>(hwnd));
            }
            return 0;
        }
        case WM_DROPFILES:
            return SendMessageW(GetParent(hwnd), msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool registerClass(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    return RegisterClassExW(&wc) != 0;
}

HWND create(HWND parent, int id) {
    HWND h = CreateWindowExW(WS_EX_ACCEPTFILES, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10,
                             parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)), nullptr);
    return h;
}

void setContent(HWND hwnd, const Content& c) {
    State* st = get(hwnd);
    if (!st) return;
    stopAnimation(hwnd, st);
    st->c = c;
    st->dirty = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void setBusy(HWND hwnd, bool busy) {
    State* st = get(hwnd);
    if (!st || st->c.busy == busy) return;
    st->c.busy = busy;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void showPhoto(HWND hwnd, bool photo) {
    State* st = get(hwnd);
    if (!st || st->photoMode == photo) return;
    stopAnimation(hwnd, st);
    st->photoMode = photo;
    st->dirty = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

}  // namespace preview
