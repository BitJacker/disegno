#include "mainwin.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

#include "../core/pipeline.h"
#include "hotkeys.h"
#include "imageio.h"
#include "library.h"
#include "mouse.h"
#include "overlay.h"
#include "preview.h"
#include "resource.h"

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const wchar_t* kMainClass = L"DisegnoMain";
const wchar_t* kAppTitle = L"Disegno";

const COLORREF kHeaderBg = RGB(79, 70, 229);
const COLORREF kHeaderSub = RGB(199, 210, 254);
const COLORREF kAccent = RGB(99, 102, 241);
const COLORREF kAccentDown = RGB(67, 56, 202);
const COLORREF kDanger = RGB(220, 38, 38);
const COLORREF kDangerDown = RGB(185, 28, 28);
const COLORREF kSep = RGB(228, 230, 237);
const COLORREF kText = RGB(31, 33, 45);
const COLORREF kMuted = RGB(107, 112, 128);

enum : int {
    ID_SEARCH = 100,
    ID_OPEN,
    ID_PASTE,
    ID_LIST,
    ID_PREVIEW,
    ID_INFO,
    ID_STYLE,
    ID_DETAIL,
    ID_DETAIL_VAL,
    ID_SHADE,
    ID_SHADE_VAL,
    ID_BRUSH,
    ID_BRUSH_UD,
    ID_INVERT,
    ID_STRETCH,
    ID_PICK,
    ID_SECS,
    ID_SECS_UD,
    ID_AREA_INFO,
    ID_SHOW_AREA,
    ID_TEST_BORDER,
    ID_SPEED,
    ID_DELAY,
    ID_DELAY_UD,
    ID_STEP,
    ID_STEP_UD,
    ID_FAILSAFE,
    ID_RELATIVE,
    ID_LIMIT,
    ID_LIMIT_SECS,
    ID_LIMIT_UD,
    ID_LBL_LIMIT_S,
    ID_DRAW,
    ID_HINT,
    ID_HELP,
    ID_STATUS,
    ID_LBL_LIBRARY,
    ID_LBL_STYLE,
    ID_LBL_DETAIL,
    ID_LBL_SHADE,
    ID_LBL_BRUSH,
    ID_LBL_BRUSH_PX,
    ID_LBL_AREA,
    ID_LBL_SECS,
    ID_LBL_SPEED,
    ID_LBL_DELAY,
    ID_LBL_DELAY_MS,
    ID_LBL_STEP,
    ID_LBL_STEP_PX,
    IDM_RENAME = 300,
    IDM_DELETE,
    IDM_EXPORT,
};

enum : UINT_PTR { TIMER_PROCESS = 1, TIMER_SAVE = 2, TIMER_SEARCH = 3 };

struct SpeedPreset {
    const char* key;
    const wchar_t* name;
    dz::Timing timing;  // stepPx, moveDelayMs, downDelayMs, upDelayMs, jiggle
};
// Games such as Roblox read the mouse once per frame (~16 ms): between two strokes the
// button has to stay released for a few frames, or the game never notices the release
// and joins every stroke to the next one with a line.
const SpeedPreset kSpeeds[] = {
    {"auto", L"Automatica  (consigliata)", {8.f, 4.f, 25.f, 25.f, false}},
    {"fast", L"Veloce  (Paint, Photoshop, Krita…)", {10.f, 1.f, 4.f, 4.f, false}},
    {"normal", L"Normale", {8.f, 4.f, 25.f, 25.f, false}},
    {"web", L"Siti web  (skribbl, Gartic…)", {12.f, 8.f, 25.f, 25.f, false}},
    {"game", L"Roblox e giochi", {24.f, 17.f, 35.f, 35.f, false}},
    {"slow", L"Molto lenta  (massima sicurezza)", {3.f, 30.f, 70.f, 70.f, true}},
    {"custom", L"Personalizzata", {6.f, 5.f, 20.f, 20.f, false}},
};
constexpr int kSpeedCount = int(sizeof kSpeeds / sizeof kSpeeds[0]);
enum : int { kAutoSpeed = 0, kFastSpeed, kNormalSpeed, kWebSpeed, kGameSpeed, kSlowSpeed, kCustomSpeed };
static_assert(kCustomSpeed == kSpeedCount - 1, "speed list and indices out of sync");

// The kind of app under the drawing area decides the automatic speed.
enum class Target { Unknown, Paint, Browser, Roblox };

const wchar_t* kStyleNames[dz::kStyleCount] = {
    L"Contorni  (veloce)",
    L"Schizzo dettagliato  (contorni + ombre)",
    L"Tratteggio  (solo ombre)",
    L"Puntini  (dettagliatissimo, lento)",
};
const char* kStyleKeys[dz::kStyleCount] = {"contorni", "schizzo", "tratteggio", "puntini"};

enum class Phase { Idle, Picking, Countdown, Drawing };
enum class After { None, Draw };

struct ProcJob {
    uint64_t gen = 0;
    std::shared_ptr<const dz::Gray> img;
    float w = 0, h = 0;
    dz::Params params;
    std::shared_ptr<std::atomic<bool>> cancel;
    HWND notify = nullptr;
};

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND status = nullptr, tooltip = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, fontBold = nullptr, fontSection = nullptr, fontTitle = nullptr, fontDraw = nullptr;
    HBRUSH brWhite = nullptr;
    HICON headerIcon = nullptr;
    HIMAGELIST thumbs = nullptr;
    int thumbSize = 88;

    // Controls
    HWND search{}, btnOpen{}, btnPaste{}, list{}, preview{}, info{};
    HWND style{}, detail{}, detailVal{}, shade{}, shadeVal{}, brush{}, brushUd{}, invert{}, stretch{};
    HWND pick{}, secs{}, secsUd{}, areaInfo{}, showArea{}, testBorder{};
    HWND speed{}, delay{}, delayUd{}, step{}, stepUd{}, failsafe{}, relative{};
    HWND limit{}, limitEdit{}, limitUd{}, lblLimitS{};
    HWND draw{}, hint{}, help{};
    HWND lblLibrary{}, lblStyle{}, lblDetail{}, lblShade{}, lblBrush{}, lblBrushPx{};
    HWND lblArea{}, lblSecs{}, lblSpeed{}, lblDelay{}, lblDelayMs{}, lblStep{}, lblStepPx{};
    int sepY[2] = {0, 0};
    int leftW = 0, rightW = 0;

    // Data
    Library lib;
    std::vector<LibItem> items;
    int64_t currentId = 0;
    std::wstring currentName;
    std::shared_ptr<const dz::Gray> gray;
    std::shared_ptr<const dz::Rgba> photo;
    RECT area{};
    bool hasArea = false;
    dz::Params params;
    int speedIndex = kAutoSpeed;
    Target target = Target::Unknown;
    std::wstring targetName;
    bool limitOn = false;
    int limitSecs = 60;
    dz::Timing custom{6.f, 5.f, 20.f, 20.f, false};
    int seconds = 5;
    bool failsafeOn = true;
    bool relativeOn = false;

    // Processing
    std::shared_ptr<const dz::Drawing> drawingFull;  // everything the picture needs
    std::shared_ptr<const dz::Drawing> drawing;      // what will be drawn (time limit applied)
    uint64_t gen = 0;
    bool procRunning = false;
    bool procPending = false;
    std::shared_ptr<std::atomic<bool>> procCancel;
    After after = After::None;

    // Drawing
    Phase phase = Phase::Idle;
    std::vector<dz::Stroke> jobStrokes;
    bool jobIsBorder = false;
    int64_t jobImageId = 0;
    ULONGLONG drawStart = 0;
    int drawTotal = 0;
    int drawDone = 0;
    double drawEstimate = 0;
    bool drawPaused = false;

    bool loadingUi = false;
};

App app;

int S(int v) { return MulDiv(v, int(app.dpi), 96); }

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(size_t(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(size_t(n));
    return s;
}

void setText(HWND h, const std::wstring& s) {
    if (getText(h) != s) SetWindowTextW(h, s.c_str());
}

int getInt(HWND edit, int def) {
    std::wstring s = getText(edit);
    if (s.empty()) return def;
    return _wtoi(s.c_str());
}

void setStatus(const std::wstring& s) {
    if (app.status) SendMessageW(app.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(s.c_str()));
}

void message(const std::wstring& text, UINT icon = MB_ICONINFORMATION) {
    MessageBoxW(app.hwnd, text.c_str(), kAppTitle, MB_OK | icon);
}

HWND makeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, app.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), FALSE);
    return h;
}

HWND makeLabel(const wchar_t* text, int id, DWORD align = SS_LEFT) {
    return makeCtl(L"STATIC", text, SS_NOPREFIX | align, id);
}

HWND makeUpDown(int id, HWND buddy, int lo, int hi, int pos) {
    HWND ud = CreateWindowExW(0, UPDOWN_CLASSW, nullptr,
                              WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                              0, 0, 0, 0, app.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.inst,
                              nullptr);
    SendMessageW(ud, UDM_SETBUDDY, reinterpret_cast<WPARAM>(buddy), 0);
    SendMessageW(ud, UDM_SETRANGE32, WPARAM(lo), LPARAM(hi));
    SendMessageW(ud, UDM_SETPOS32, 0, LPARAM(pos));
    return ud;
}

void addTip(HWND ctl, const wchar_t* text) {
    if (!app.tooltip || !ctl) return;
    TTTOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = app.hwnd;
    ti.uId = reinterpret_cast<UINT_PTR>(ctl);
    ti.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(app.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

bool isSection(int id) {
    return id == ID_LBL_LIBRARY || id == ID_LBL_STYLE || id == ID_LBL_AREA || id == ID_LBL_SPEED;
}

void bringToFront() {
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    if (IsIconic(app.hwnd)) ShowWindow(app.hwnd, SW_RESTORE);
    else ShowWindow(app.hwnd, SW_SHOW);
    SetForegroundWindow(app.hwnd);
    BringWindowToTop(app.hwnd);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
}

bool isCloaked(HWND h) {
    BOOL cloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof cloaked)) && cloaked;
}

// Top-most visible window (not ours) under a screen point.
HWND windowAt(POINT pt) {
    struct Find {
        POINT pt;
        HWND found;
    } search{pt, nullptr};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* f = reinterpret_cast<Find*>(lp);
            if (h == app.hwnd || !IsWindowVisible(h) || IsIconic(h)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId()) return TRUE;
            if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT) return TRUE;
            if (isCloaked(h)) return TRUE;
            RECT r;
            if (GetWindowRect(h, &r) && PtInRect(&r, f->pt)) {
                f->found = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

std::wstring guessExtension(const Bytes& d) {
    auto has = [&](size_t off, const char* sig) {
        size_t n = strlen(sig);
        return d.size() >= off + n && memcmp(d.data() + off, sig, n) == 0;
    };
    if (has(0, "\x89PNG")) return L".png";
    if (has(0, "\xFF\xD8\xFF")) return L".jpg";
    if (has(0, "GIF8")) return L".gif";
    if (has(0, "BM")) return L".bmp";
    if (has(0, "RIFF") && has(8, "WEBP")) return L".webp";
    if (has(0, "II*") || has(0, "MM")) return L".tif";
    if (has(4, "ftyp")) return L".heic";
    return L".img";
}

// ---------------------------------------------------------------------------
// Timing / sizes
// ---------------------------------------------------------------------------

// The preset really used: "Automatica" follows the app under the drawing area.
int effectiveSpeed() {
    if (app.speedIndex != kAutoSpeed) return std::clamp(app.speedIndex, 0, kSpeedCount - 1);
    switch (app.target) {
        case Target::Roblox: return kGameSpeed;
        case Target::Browser: return kWebSpeed;
        case Target::Paint: return kFastSpeed;
        default: return kNormalSpeed;
    }
}

dz::Timing currentTiming() {
    const int s = effectiveSpeed();
    if (s == kCustomSpeed) {
        dz::Timing t = app.custom;
        // Keep the button released for at least a couple of game frames between strokes.
        t.downDelayMs = t.upDelayMs = std::clamp(t.moveDelayMs * 2.5f, 25.f, 150.f);
        t.jiggle = t.moveDelayMs >= 12.f;
        return t;
    }
    return kSpeeds[s].timing;
}

std::wstring lowerCase(std::wstring s) {
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}

// Classifies the application that owns a window (by process name and title).
Target classifyWindow(HWND h, std::wstring& label) {
    label.clear();
    if (!h) return Target::Unknown;
    HWND root = GetAncestor(h, GA_ROOT);
    if (!root) root = h;
    wchar_t title[256] = {};
    GetWindowTextW(root, title, 256);
    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    std::wstring exe;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = DWORD(std::size(buf));
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) exe.assign(buf, n);
        CloseHandle(p);
    }
    const size_t slash = exe.find_last_of(L"\\/");
    const std::wstring name = lowerCase(slash == std::wstring::npos ? exe : exe.substr(slash + 1));
    const std::wstring t = lowerCase(title);
    label = title[0] ? std::wstring(title) : name;
    if (name.find(L"roblox") != std::wstring::npos || t.find(L"roblox") != std::wstring::npos) {
        label = L"Roblox";
        return Target::Roblox;
    }
    static const wchar_t* browsers[] = {L"chrome.exe",  L"msedge.exe",   L"firefox.exe",  L"opera.exe",
                                        L"brave.exe",   L"vivaldi.exe",  L"iexplore.exe", L"arc.exe",
                                        L"librewolf.exe", L"waterfox.exe", L"yandex.exe",  L"browser.exe",
                                        L"opera_gx.exe", L"chromium.exe"};
    for (const wchar_t* b : browsers)
        if (name == b) return Target::Browser;
    static const wchar_t* painters[] = {L"mspaint.exe",   L"paintstudio.view.exe", L"krita.exe",
                                        L"photoshop.exe", L"paintdotnet.exe",      L"clipstudiopaint.exe",
                                        L"sai.exe",       L"sai2.exe",             L"medibangpaintpro.exe",
                                        L"firealpaca.exe", L"artweaver.exe",       L"mypaint.exe",
                                        L"aseprite.exe",  L"inkscape.exe",         L"photopea.exe"};
    for (const wchar_t* pa : painters)
        if (name == pa) return Target::Paint;
    if (name.rfind(L"gimp", 0) == 0 || t.find(L"paint") != std::wstring::npos) return Target::Paint;
    return Target::Unknown;
}

// Looks at which app is under the middle of the drawing area. Returns true if it changed.
bool detectTarget() {
    if (!app.hasArea) return false;
    const POINT c{(app.area.left + app.area.right) / 2, (app.area.top + app.area.bottom) / 2};
    std::wstring label;
    const Target t = classifyWindow(windowAt(c), label);
    const bool changed = t != app.target || label != app.targetName;
    app.target = t;
    app.targetName = label;
    return changed;
}

// Size the drawing is built for. Before an area is chosen, a sheet shaped like the photo.
void areaSize(float& w, float& h) {
    if (app.hasArea) {
        w = float(app.area.right - app.area.left);
        h = float(app.area.bottom - app.area.top);
        return;
    }
    w = 800.f;
    h = 600.f;
    if (app.gray && app.gray->w > 0) h = std::clamp(800.f * float(app.gray->h) / float(app.gray->w), 300.f, 1200.f);
}

bool drawingIsCurrent() {
    if (!app.drawing || app.procRunning || app.procPending) return false;
    float w, h;
    areaSize(w, h);
    return std::fabs(app.drawing->width - w) < 0.5f && std::fabs(app.drawing->height - h) < 0.5f;
}

// ---------------------------------------------------------------------------
// Display updates
// ---------------------------------------------------------------------------

void updatePreview() {
    preview::Content c;
    areaSize(c.areaW, c.areaH);
    if (app.drawing && std::fabs(app.drawing->width - c.areaW) < 0.5f && std::fabs(app.drawing->height - c.areaH) < 0.5f)
        c.drawing = app.drawing;
    c.photo = app.photo;
    c.stretch = app.params.stretch;
    c.busy = app.procRunning || app.procPending;
    preview::setContent(app.preview, c);
}

void updateInfo() {
    std::wstring s;
    if (!app.gray) {
        s = L"Nessuna foto selezionata";
    } else if (!drawingIsCurrent()) {
        s = L"Preparazione del disegno…";
    } else {
        const size_t n = app.drawing->strokes.size();
        const size_t all = app.drawingFull ? app.drawingFull->strokes.size() : n;
        const double sec = dz::estimateSeconds(app.drawing->strokes, currentTiming());
        s = wu::formatInt(static_cast<long long>(n));
        if (n < all) s += L" di " + wu::formatInt(static_cast<long long>(all));
        s += L" tratti   ·   tempo stimato " + wu::formatDuration(sec);
        if (n < all) s += L" (massimo " + wu::formatDuration(app.limitSecs) + L")";
        if (app.speedIndex == kAutoSpeed) s += L"   ·   velocità: " + std::wstring(kSpeeds[effectiveSpeed()].name);
        if (!app.hasArea) s += L"   ·   scegli l'area per l'anteprima esatta";
    }
    setText(app.info, s);
}

// Applies the optional time limit to the full drawing.
void applyTimeLimit() {
    if (!app.drawingFull) {
        app.drawing.reset();
        return;
    }
    if (!app.limitOn) {
        app.drawing = app.drawingFull;
        return;
    }
    auto d = std::make_shared<dz::Drawing>();
    d->width = app.drawingFull->width;
    d->height = app.drawingFull->height;
    d->brush = app.drawingFull->brush;
    // Keep 10% in reserve: games can run a little slower than the estimate.
    d->strokes = dz::fitToTime(app.drawingFull->strokes, currentTiming(), double(app.limitSecs) * 0.9);
    app.drawing = d;
}

void updateAreaInfo() {
    if (!app.hasArea) {
        setText(app.areaInfo, L"Nessuna area scelta");
    } else {
        const RECT& a = app.area;
        std::wstring s = std::to_wstring(a.right - a.left) + L" × " + std::to_wstring(a.bottom - a.top) + L" px  ·  (" +
                         std::to_wstring(a.left) + L", " + std::to_wstring(a.top) + L")";
        if (!app.targetName.empty()) s += L"  ·  " + app.targetName;
        setText(app.areaInfo, s);
    }
    EnableWindow(app.showArea, app.hasArea);
    EnableWindow(app.testBorder, app.hasArea);
}

void updateSpeedFields() {
    const int eff = effectiveSpeed();
    const dz::Timing t = eff == kCustomSpeed ? app.custom : kSpeeds[eff].timing;
    app.loadingUi = true;
    SendMessageW(app.delayUd, UDM_SETPOS32, 0, LPARAM(std::lround(t.moveDelayMs)));
    SendMessageW(app.stepUd, UDM_SETPOS32, 0, LPARAM(std::lround(t.stepPx)));
    app.loadingUi = false;
    const bool custom = app.speedIndex == kCustomSpeed;
    EnableWindow(app.delay, custom);
    EnableWindow(app.delayUd, custom);
    EnableWindow(app.step, custom);
    EnableWindow(app.stepUd, custom);
}

void updateValueLabels() {
    setText(app.detailVal, std::to_wstring(app.params.detail));
    setText(app.shadeVal, std::to_wstring(app.params.shading));
    const bool shadingUsed = app.params.style != dz::Style::Outline;
    EnableWindow(app.shade, shadingUsed);
    EnableWindow(app.lblShade, shadingUsed);
    EnableWindow(app.shadeVal, shadingUsed);
}

void syncControls() {
    app.loadingUi = true;
    SendMessageW(app.style, CB_SETCURSEL, WPARAM(int(app.params.style)), 0);
    SendMessageW(app.detail, TBM_SETPOS, TRUE, app.params.detail);
    SendMessageW(app.shade, TBM_SETPOS, TRUE, app.params.shading);
    SendMessageW(app.brushUd, UDM_SETPOS32, 0, LPARAM(std::lround(app.params.brush)));
    Button_SetCheck(app.invert, app.params.invert ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(app.stretch, app.params.stretch ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(app.secsUd, UDM_SETPOS32, 0, app.seconds);
    SendMessageW(app.speed, CB_SETCURSEL, WPARAM(app.speedIndex), 0);
    Button_SetCheck(app.failsafe, app.failsafeOn ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(app.relative, app.relativeOn ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(app.limit, app.limitOn ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(app.limitUd, UDM_SETPOS32, 0, app.limitSecs);
    EnableWindow(app.limitEdit, app.limitOn);
    EnableWindow(app.limitUd, app.limitOn);
    app.loadingUi = false;
    updateSpeedFields();
    updateValueLabels();
    updateAreaInfo();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

std::string imageSettingsString() {
    char buf[160];
    snprintf(buf, sizeof buf, "s=%d;d=%d;h=%d;b=%d;i=%d;f=%d", int(app.params.style), app.params.detail,
             app.params.shading, int(std::lround(app.params.brush)), app.params.invert ? 1 : 0,
             app.params.stretch ? 1 : 0);
    return buf;
}

void applyImageSettings(const std::string& s) {
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(';', pos);
        if (end == std::string::npos) end = s.size();
        std::string kv = s.substr(pos, end - pos);
        pos = end + 1;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string k = kv.substr(0, eq);
        int v = atoi(kv.c_str() + eq + 1);
        if (k == "s") app.params.style = dz::Style(std::clamp(v, 0, dz::kStyleCount - 1));
        else if (k == "d") app.params.detail = std::clamp(v, 1, 10);
        else if (k == "h") app.params.shading = std::clamp(v, 0, 10);
        else if (k == "b") app.params.brush = float(std::clamp(v, 1, 40));
        else if (k == "i") app.params.invert = v != 0;
        else if (k == "f") app.params.stretch = v != 0;
    }
}

int settingInt(const char* key, int def, int lo, int hi) {
    std::string v = app.lib.setting(key);
    return v.empty() ? def : std::clamp(atoi(v.c_str()), lo, hi);
}

void loadSettings() {
    applyImageSettings(app.lib.setting("params", "s=1;d=6;h=5;b=2;i=0;f=0"));
    {
        // Stored by name; very old versions stored an index (their fast default becomes "auto").
        const std::string sp = app.lib.setting("speed", "auto");
        app.speedIndex = kAutoSpeed;
        if (!sp.empty() && isdigit(static_cast<unsigned char>(sp[0]))) {
            static const int legacy[] = {kAutoSpeed, kNormalSpeed, kWebSpeed, kGameSpeed, kSlowSpeed, kCustomSpeed};
            const int v = atoi(sp.c_str());
            if (v >= 0 && v < 6) app.speedIndex = legacy[v];
        } else {
            for (int i = 0; i < kSpeedCount; ++i)
                if (sp == kSpeeds[i].key) app.speedIndex = i;
        }
    }
    app.limitOn = settingInt("limit_on", 0, 0, 1) != 0;
    app.limitSecs = settingInt("limit_secs", 60, 5, 3600);
    app.custom.moveDelayMs = float(settingInt("custom_delay", 5, 0, 500));
    app.custom.stepPx = float(settingInt("custom_step", 6, 1, 100));
    app.seconds = settingInt("seconds", 5, 1, 30);
    app.failsafeOn = settingInt("failsafe", 1, 0, 1) != 0;
    app.relativeOn = settingInt("relative", 0, 0, 1) != 0;
    std::string a = app.lib.setting("area");
    RECT r{};
    if (sscanf(a.c_str(), "%ld,%ld,%ld,%ld", &r.left, &r.top, &r.right, &r.bottom) == 4 && r.right - r.left >= 8 &&
        r.bottom - r.top >= 8) {
        app.area = r;
        app.hasArea = true;
    }
}

void saveSettingsNow() {
    KillTimer(app.hwnd, TIMER_SAVE);
    if (!app.lib.isOpen()) return;
    app.lib.begin();
    app.lib.setSetting("params", imageSettingsString());
    app.lib.setSetting("speed", kSpeeds[app.speedIndex].key);
    app.lib.setSetting("limit_on", app.limitOn ? "1" : "0");
    app.lib.setSetting("limit_secs", std::to_string(app.limitSecs));
    app.lib.setSetting("custom_delay", std::to_string(int(std::lround(app.custom.moveDelayMs))));
    app.lib.setSetting("custom_step", std::to_string(int(std::lround(app.custom.stepPx))));
    app.lib.setSetting("seconds", std::to_string(app.seconds));
    app.lib.setSetting("failsafe", app.failsafeOn ? "1" : "0");
    app.lib.setSetting("relative", app.relativeOn ? "1" : "0");
    if (app.hasArea) {
        char buf[96];
        snprintf(buf, sizeof buf, "%ld,%ld,%ld,%ld", app.area.left, app.area.top, app.area.right, app.area.bottom);
        app.lib.setSetting("area", buf);
    }
    app.lib.setSetting("last_image", std::to_string(app.currentId));
    if (app.currentId) app.lib.setImageSettings(app.currentId, imageSettingsString());
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    if (GetWindowPlacement(app.hwnd, &wp)) {
        char buf[128];
        const RECT& r = wp.rcNormalPosition;
        snprintf(buf, sizeof buf, "%ld,%ld,%ld,%ld,%d", r.left, r.top, r.right, r.bottom,
                 wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
        app.lib.setSetting("window", buf);
    }
    app.lib.commit();
}

void saveSettingsLater() { SetTimer(app.hwnd, TIMER_SAVE, 800, nullptr); }

// ---------------------------------------------------------------------------
// Processing (photo -> strokes) on a worker thread
// ---------------------------------------------------------------------------

DWORD WINAPI procThread(LPVOID arg) {
    std::unique_ptr<ProcJob> job(static_cast<ProcJob*>(arg));
    dz::Drawing* d = nullptr;
    try {
        d = new dz::Drawing();
        if (!dz::buildDrawing(*job->img, job->w, job->h, job->params, *d, job->cancel.get())) {
            delete d;
            d = nullptr;
        }
    } catch (...) {
        delete d;
        d = nullptr;
    }
    if (!PostMessageW(job->notify, WM_APP_PROCESSED, WPARAM(job->gen), reinterpret_cast<LPARAM>(d))) delete d;
    return 0;
}

void startProcess() {
    KillTimer(app.hwnd, TIMER_PROCESS);
    if (!app.gray) {
        app.drawingFull.reset();
        app.drawing.reset();
        updatePreview();
        updateInfo();
        return;
    }
    if (app.procRunning) {
        app.procPending = true;
        if (app.procCancel) *app.procCancel = true;
        preview::setBusy(app.preview, true);
        return;
    }
    auto* job = new ProcJob();
    job->gen = ++app.gen;
    job->img = app.gray;
    areaSize(job->w, job->h);
    job->params = app.params;
    job->cancel = std::make_shared<std::atomic<bool>>(false);
    job->notify = app.hwnd;
    app.procCancel = job->cancel;
    HANDLE th = CreateThread(nullptr, 0, procThread, job, 0, nullptr);
    if (!th) {
        delete job;
        return;
    }
    CloseHandle(th);
    app.procRunning = true;
    preview::setBusy(app.preview, true);
    updateInfo();
}

void scheduleProcess(UINT delayMs = 200) {
    if (!app.gray) return;
    preview::setBusy(app.preview, true);
    SetTimer(app.hwnd, TIMER_PROCESS, delayMs, nullptr);
}

void beginCountdown(bool border);

void onProcessed(uint64_t gen, dz::Drawing* d) {
    std::unique_ptr<dz::Drawing> result(d);
    app.procRunning = false;
    if (gen == app.gen && result) {
        app.drawingFull = std::shared_ptr<const dz::Drawing>(result.release());
        applyTimeLimit();
    }
    if (app.procPending) {
        app.procPending = false;
        startProcess();
        return;
    }
    updatePreview();
    updateInfo();
    if (app.after == After::Draw && drawingIsCurrent()) {
        app.after = After::None;
        beginCountdown(false);
    }
}

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------

HBITMAP makeTile(const Bytes& png, int T) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = T;
    bi.bmiHeader.biHeight = -T;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return nullptr;
    auto* px = static_cast<uint32_t*>(bits);
    std::fill(px, px + size_t(T) * T, 0xFFF1F2F6u);
    dz::Rgba th;
    if (!png.empty() && imgio::decode(png, th, 1024)) {
        const int box = T - S(6);
        double s = std::min(double(box) / th.w, double(box) / th.h);
        int w = std::max(1, int(th.w * s + 0.5)), h = std::max(1, int(th.h * s + 0.5));
        dz::Rgba t = dz::resize(th, w, h);
        int ox = (T - w) / 2, oy = (T - h) / 2;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t* p = &t.p[(size_t(y) * w + x) * 4];
                uint32_t a = p[3];
                uint32_t r = (p[0] * a + 0xF1 * (255 - a)) / 255;
                uint32_t g = (p[1] * a + 0xF2 * (255 - a)) / 255;
                uint32_t b = (p[2] * a + 0xF6 * (255 - a)) / 255;
                px[size_t(oy + y) * T + (ox + x)] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
    }
    return bmp;
}

void recreateThumbList() {
    app.thumbSize = S(88);
    HIMAGELIST old = app.thumbs;
    app.thumbs = ImageList_Create(app.thumbSize, app.thumbSize, ILC_COLOR32, 16, 16);
    ListView_SetImageList(app.list, app.thumbs, LVSIL_NORMAL);
    ListView_SetIconSpacing(app.list, app.thumbSize + S(8), app.thumbSize + S(38));
    if (old) ImageList_Destroy(old);
}

int selectedIndex() { return ListView_GetNextItem(app.list, -1, LVNI_SELECTED); }

void refreshLibrary(int64_t selectId) {
    app.items = app.lib.list(getText(app.search));
    app.loadingUi = true;
    SendMessageW(app.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(app.list);
    ImageList_RemoveAll(app.thumbs);
    int sel = -1;
    for (size_t i = 0; i < app.items.size(); ++i) {
        const LibItem& it = app.items[i];
        Bytes png;
        app.lib.thumb(it.id, png);
        HBITMAP bmp = makeTile(png, app.thumbSize);
        int img = bmp ? ImageList_Add(app.thumbs, bmp, nullptr) : -1;
        if (bmp) DeleteObject(bmp);
        LVITEMW lv{};
        lv.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lv.iItem = int(i);
        lv.pszText = const_cast<LPWSTR>(it.name.c_str());
        lv.iImage = img;
        lv.lParam = LPARAM(it.id);
        ListView_InsertItem(app.list, &lv);
        if (it.id == selectId) sel = int(i);
    }
    if (sel >= 0) {
        ListView_SetItemState(app.list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(app.list, sel, FALSE);
    }
    SendMessageW(app.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(app.list, nullptr, TRUE);
    app.loadingUi = false;
}

void clearCurrentImage() {
    app.currentId = 0;
    app.currentName.clear();
    app.gray.reset();
    app.photo.reset();
    app.drawingFull.reset();
    app.drawing.reset();
    SetWindowTextW(app.hwnd, kAppTitle);
    updatePreview();
    updateInfo();
}

void setImage(int64_t id, const dz::Rgba& rgba, const std::wstring& name) {
    const double sp = std::min(1.0, 2048.0 / std::max(rgba.w, rgba.h));
    dz::Rgba work = sp < 1 ? dz::resize(rgba, std::max(1, int(rgba.w * sp + 0.5)), std::max(1, int(rgba.h * sp + 0.5)))
                           : rgba;
    app.gray = std::make_shared<const dz::Gray>(dz::toGray(work));
    const double pp = std::min(1.0, 1400.0 / std::max(work.w, work.h));
    app.photo = std::make_shared<const dz::Rgba>(
        pp < 1 ? dz::resize(work, std::max(1, int(work.w * pp + 0.5)), std::max(1, int(work.h * pp + 0.5))) : work);
    app.currentId = id;
    app.currentName = name;
    app.drawingFull.reset();
    app.drawing.reset();
    const std::string st = app.lib.imageSettings(id);
    if (!st.empty()) {
        applyImageSettings(st);
        syncControls();
    }
    app.lib.touch(id);
    SetWindowTextW(app.hwnd, (name + L"  –  Disegno").c_str());
    setStatus(L"Foto: " + name + L"  (" + std::to_wstring(rgba.w) + L" × " + std::to_wstring(rgba.h) + L")");
    saveSettingsLater();
    updatePreview();
    startProcess();
}

bool loadFromLibrary(int64_t id) {
    Bytes data;
    if (!app.lib.data(id, data)) {
        message(L"Non riesco a leggere questa foto dal database.", MB_ICONWARNING);
        return false;
    }
    dz::Rgba rgba;
    if (!imgio::decode(data, rgba)) {
        message(L"Questa foto è danneggiata o in un formato non supportato.", MB_ICONWARNING);
        return false;
    }
    LibItem it;
    app.lib.item(id, it);
    setImage(id, rgba, it.name);
    return true;
}

bool addImage(const Bytes& data, const std::wstring& name, const std::wstring& source, bool select) {
    dz::Rgba rgba;
    if (!imgio::decode(data, rgba)) {
        message(L"Questo file non sembra un'immagine valida:\n" + name, MB_ICONWARNING);
        return false;
    }
    Bytes stored = data;
    if (stored.size() > 40u * 1024u * 1024u) imgio::encodePng(rgba, stored);  // huge files: keep a decoded copy
    const Bytes thumb = imgio::makeThumbnail(rgba, 192);
    bool existed = false;
    const int64_t id = app.lib.add(name.empty() ? L"Foto" : name, source, stored, rgba.w, rgba.h, thumb, &existed);
    if (!id) {
        message(L"Non riesco a salvare la foto nella libreria.", MB_ICONWARNING);
        return false;
    }
    if (select) {
        LibItem it;
        std::wstring shown = existed && app.lib.item(id, it) ? it.name : name;
        setImage(id, rgba, shown);
        if (existed) setStatus(L"Questa foto era già nella libreria: «" + shown + L"»");
    }
    app.loadingUi = true;
    SetWindowTextW(app.search, L"");
    app.loadingUi = false;
    refreshLibrary(select ? id : app.currentId);
    return true;
}

void openFiles(const std::vector<std::wstring>& paths) {
    for (size_t i = 0; i < paths.size(); ++i) {
        Bytes data;
        if (!wu::readFile(paths[i], data)) {
            message(L"Impossibile leggere il file:\n" + paths[i], MB_ICONWARNING);
            continue;
        }
        addImage(data, wu::fileStem(paths[i]), paths[i], i + 1 == paths.size());
    }
}

void openDialog() {
    std::vector<std::wstring> files = imgio::pickImages(app.hwnd);
    if (!files.empty()) openFiles(files);
}

void pasteImage() {
    Bytes data;
    std::wstring name, source, err;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = imgio::fromClipboard(app.hwnd, data, name, source, &err);
    SetCursor(old);
    if (!ok) {
        message(err);
        return;
    }
    addImage(data, name, source, true);
}

void deleteSelected() {
    int idx = selectedIndex();
    if (idx < 0 || idx >= int(app.items.size())) return;
    const LibItem it = app.items[size_t(idx)];
    std::wstring q = L"Eliminare «" + it.name + L"» dalla libreria?";
    if (MessageBoxW(app.hwnd, q.c_str(), kAppTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    app.lib.remove(it.id);
    if (it.id == app.currentId) clearCurrentImage();
    refreshLibrary(app.currentId);
    setStatus(L"Foto eliminata: " + it.name);
}

void exportSelected() {
    int idx = selectedIndex();
    if (idx < 0 || idx >= int(app.items.size())) return;
    const LibItem it = app.items[size_t(idx)];
    Bytes data;
    if (!app.lib.data(it.id, data)) return;
    std::wstring path;
    if (!imgio::pickSavePath(app.hwnd, it.name + guessExtension(data), path)) return;
    if (wu::writeFile(path, data.data(), data.size())) setStatus(L"Esportata in " + path);
    else message(L"Non riesco a scrivere il file:\n" + path, MB_ICONWARNING);
}

// ---------------------------------------------------------------------------
// Area / drawing flow
// ---------------------------------------------------------------------------

void setPhase(Phase p) {
    app.phase = p;
    // ESC / F8 are watched system-wide only while picking, counting down or drawing.
    if (p == Phase::Idle) hotkeys::disable();
    else hotkeys::enable();
    InvalidateRect(app.draw, nullptr, FALSE);
}

void startPick() {
    if (app.phase != Phase::Idle) return;
    setPhase(Phase::Picking);
    setStatus(L"Porta il mouse nell'angolo in alto a sinistra, poi in basso a destra…");
    ShowWindow(app.hwnd, SW_MINIMIZE);
    overlay::pickArea(app.hwnd, app.seconds);
}

void onAreaPicked(bool ok, RECT* r) {
    std::unique_ptr<RECT> rect(r);
    setPhase(Phase::Idle);
    bringToFront();
    if (!ok || !rect) {
        setStatus(L"Selezione dell'area annullata.");
        return;
    }
    app.area = *rect;
    app.hasArea = true;
    detectTarget();
    updateAreaInfo();
    updateSpeedFields();
    saveSettingsLater();
    std::wstring msg = L"Area scelta: " + std::to_wstring(rect->right - rect->left) + L" × " +
                       std::to_wstring(rect->bottom - rect->top) + L" px";
    if (app.speedIndex == kAutoSpeed && app.target != Target::Unknown)
        msg += L", in " + app.targetName + L" (velocità «" + kSpeeds[effectiveSpeed()].name + L"»)";
    setStatus(msg + L". Ora premi DISEGNA.");
    app.drawingFull.reset();
    app.drawing.reset();
    updatePreview();
    startProcess();
}

std::vector<dz::Stroke> borderStrokes() {
    const float w = float(app.area.right - app.area.left), h = float(app.area.bottom - app.area.top);
    const float b = std::min(std::max(0.f, std::floor(app.params.brush / 2.f)), std::min(w, h) / 4.f);
    dz::Stroke s;
    s.pts = {{b, b}, {w - b, b}, {w - b, h - b}, {b, h - b}, {b, b}};
    return {s};
}

void beginCountdown(bool border) {
    if (app.phase != Phase::Idle || !app.hasArea) return;
    if (!border && !app.drawing) return;
    if (detectTarget()) {
        // Another app is under the area now: the automatic speed (and time limit) may change.
        applyTimeLimit();
        updateAreaInfo();
        updateSpeedFields();
        updatePreview();
        updateInfo();
    }
    app.jobIsBorder = border;
    app.jobImageId = app.currentId;
    app.jobStrokes = border ? borderStrokes() : app.drawing->strokes;
    app.drawEstimate = dz::estimateSeconds(app.jobStrokes, currentTiming());
    setPhase(Phase::Countdown);
    setStatus(L"Il disegno parte tra " + std::to_wstring(app.seconds) + L" secondi (velocità «" +
              kSpeeds[effectiveSpeed()].name + L"»)…");
    // Give focus to the app under the area while we are still allowed to, then get out of the way.
    POINT c{(app.area.left + app.area.right) / 2, (app.area.top + app.area.bottom) / 2};
    if (HWND target = windowAt(c)) SetForegroundWindow(target);
    ShowWindow(app.hwnd, SW_MINIMIZE);
    overlay::countdown(app.hwnd, app.area, app.seconds);
}

void onDrawClicked() {
    switch (app.phase) {
        case Phase::Picking:
        case Phase::Countdown:
            overlay::cancel();
            return;
        case Phase::Drawing:
            mouse::requestStop();
            return;
        case Phase::Idle:
            break;
    }
    if (!app.gray) {
        message(L"Prima scegli una foto: aprila con «Apri foto…», incollala con Ctrl+V oppure scegline una "
                L"dalla libreria.");
        return;
    }
    if (!app.hasArea) {
        if (MessageBoxW(app.hwnd,
                        L"Non hai ancora scelto dove disegnare.\n\nVuoi selezionare l'area adesso? Avrai "
                        L"qualche secondo per l'angolo in alto a sinistra e poi per quello in basso a destra.",
                        kAppTitle, MB_YESNO | MB_ICONQUESTION) == IDYES)
            startPick();
        return;
    }
    if (!drawingIsCurrent()) {
        app.after = After::Draw;
        setStatus(L"Preparo il disegno…");
        if (!app.procRunning) startProcess();
        return;
    }
    if (app.drawing->strokes.empty()) {
        message(L"Con queste impostazioni non c'è niente da disegnare.\nProva ad aumentare il dettaglio o le ombre.");
        return;
    }
    beginCountdown(false);
}

void onCountdownDone(bool ok) {
    if (!ok) {
        setPhase(Phase::Idle);
        app.jobStrokes.clear();
        bringToFront();
        setStatus(L"Disegno annullato.");
        return;
    }
    auto* job = new DrawJob();
    job->strokes = std::move(app.jobStrokes);
    app.jobStrokes.clear();
    job->origin = POINT{app.area.left, app.area.top};
    job->timing = currentTiming();
    job->failsafe = app.failsafeOn;
    job->relative = app.relativeOn;
    job->notify = app.hwnd;
    app.drawTotal = int(job->strokes.size());
    app.drawDone = 0;
    app.drawPaused = false;
    app.drawStart = GetTickCount64();
    if (!mouse::start(job)) {
        setPhase(Phase::Idle);
        bringToFront();
        message(L"Non riesco ad avviare il disegno.", MB_ICONWARNING);
        return;
    }
    setPhase(Phase::Drawing);
    setStatus(L"Sto disegnando…  ESC = ferma, F8 = pausa");
    overlay::showStatus(app.area);
    overlay::setStatus(L"Disegno in corso…  0%", L"ESC = ferma   ·   F8 = pausa   ·   non toccare il mouse", 0, false);
}

void onDrawProgress(int done, int total) {
    app.drawDone = done;
    const double frac = total > 0 ? double(done) / total : 0;
    const double elapsed = double(GetTickCount64() - app.drawStart) / 1000.0;
    double left = frac > 0.03 ? elapsed / frac - elapsed : std::max(0.0, app.drawEstimate - elapsed);
    wchar_t title[96];
    swprintf(title, 96, L"Disegno in corso…  %d%%", int(frac * 100));
    overlay::setStatus(title, L"Mancano circa " + wu::formatDuration(left) + L"   ·   ESC = ferma   ·   F8 = pausa",
                       frac, false);
}

void onDrawPaused(bool paused) {
    app.drawPaused = paused;
    const double frac = app.drawTotal > 0 ? double(app.drawDone) / app.drawTotal : 0;
    if (paused) {
        overlay::setStatus(L"In pausa", L"F8 = riprendi   ·   ESC = ferma", frac, true);
        setStatus(L"In pausa: premi F8 per riprendere.");
    } else {
        overlay::setStatus(L"Riprendo…", L"ESC = ferma   ·   F8 = pausa", frac, false);
        setStatus(L"Sto disegnando…  ESC = ferma, F8 = pausa");
    }
}

void onDrawDone(DrawResult result, int done) {
    setPhase(Phase::Idle);
    const double elapsed = double(GetTickCount64() - app.drawStart) / 1000.0;
    const double frac = app.drawTotal > 0 ? double(done) / app.drawTotal : 1;
    std::wstring title, line;
    switch (result) {
        case DrawResult::Completed:
            title = L"Finito!";
            line = wu::formatInt(done) + L" tratti in " + wu::formatDuration(elapsed);
            break;
        case DrawResult::Stopped:
            title = L"Disegno fermato";
            line = L"Fatti " + wu::formatInt(done) + L" tratti su " + wu::formatInt(app.drawTotal);
            break;
        case DrawResult::UserMoved:
            title = L"Fermato: hai mosso il mouse";
            line = L"Per sicurezza mi fermo se il mouse si muove da solo";
            break;
    }
    overlay::setStatus(title, line, frac, result != DrawResult::Completed);
    overlay::hideStatus(3500);
    setStatus(title + L" – " + line);
    if (!app.jobIsBorder && app.lib.isOpen())
        app.lib.recordDrawing(app.jobImageId, kStyleKeys[int(app.params.style)], done, elapsed,
                              result == DrawResult::Completed);
    FLASHWINFO fi{sizeof fi, app.hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
    FlashWindowEx(&fi);
    MessageBeep(result == DrawResult::Completed ? MB_OK : MB_ICONEXCLAMATION);
}

void showHelp() {
    message(
        L"DISEGNO " + wu::widen(DISEGNO_VERSION_STR) + L"  –  COME SI USA\n\n"
        L"1.  Apri una foto con «Apri foto…», oppure copiala e premi Ctrl+V (va bene anche il link di "
        L"un'immagine). Resta salvata nella libreria a sinistra.\n"
        L"2.  Scegli stile, dettaglio e ombre: l'anteprima mostra esattamente cosa verrà disegnato.\n"
        L"3.  Apri l'app dove vuoi disegnare (Paint, Roblox, un sito…) e scegli matita o pennello.\n"
        L"4.  Premi «Seleziona area»: hai qualche secondo per mettere il mouse nell'angolo IN ALTO A "
        L"SINISTRA del foglio, poi nell'angolo IN BASSO A DESTRA.\n"
        L"5.  Premi DISEGNA e non toccare il mouse finché non ha finito.\n\n"
        L"TASTI\n"
        L"ESC = ferma subito      F8 = pausa / riprendi\n"
        L"F5 = disegna      F6 = seleziona area      Ctrl+V = incolla foto\n\n"
        L"CONSIGLI\n"
        L"•  La velocità «Automatica» riconosce Paint, i browser e Roblox e sceglie i tempi giusti.\n"
        L"•  Roblox e altri giochi leggono il mouse una volta per fotogramma: usa «Roblox e giochi» "
        L"(o «Automatica»), stile Contorni o poco dettaglio, e lo spessore uguale al pennello del gioco.\n"
        L"•  Giochi a tempo: attiva «Tempo massimo» e Disegno toglie i tratti meno importanti per "
        L"finire in tempo.\n"
        L"•  Se un gioco ignora il mouse prova «Movimento relativo».\n"
        L"•  Se l'app di disegno è avviata come amministratore, avvia anche Disegno come amministratore.");
}

// ---------------------------------------------------------------------------
// Layout and painting
// ---------------------------------------------------------------------------

int headerHeight() { return S(52); }

void layout() {
    RECT rc;
    GetClientRect(app.hwnd, &rc);
    SendMessageW(app.status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(app.status, &sr);
    const int statusH = sr.bottom - sr.top;
    const int W = rc.right, top = headerHeight(), bottom = rc.bottom - statusH;
    const int pad = S(12);
    app.leftW = S(236);
    app.rightW = S(320);

    HDWP dw = BeginDeferWindowPos(48);
    auto put = [&](HWND h, int x, int y, int w, int hh) {
        if (h) dw = DeferWindowPos(dw, h, nullptr, x, y, std::max(0, w), std::max(0, hh), SWP_NOZORDER | SWP_NOACTIVATE);
    };

    put(app.help, W - pad - S(128), (top - S(30)) / 2, S(128), S(30));

    // Left: library
    {
        int x = pad, y = top + S(12), w = app.leftW - 2 * pad;
        put(app.lblLibrary, x, y, w, S(18));
        y += S(22);
        put(app.search, x, y, w, S(24));
        y += S(30);
        int bw = (w - S(6)) / 2;
        put(app.btnOpen, x, y, bw, S(30));
        put(app.btnPaste, x + bw + S(6), y, w - bw - S(6), S(30));
        y += S(38);
        put(app.list, x, y, w, bottom - pad - y);
    }

    // Right: settings
    {
        const int rx = W - app.rightW + pad, rw = app.rightW - 2 * pad;
        int y = top + S(10);
        put(app.lblStyle, rx, y, rw, S(18));
        y += S(20);
        put(app.style, rx, y, rw, S(300));
        y += S(30);
        const int lw = S(76), vw = S(26);
        put(app.lblDetail, rx, y + S(4), lw, S(20));
        put(app.detail, rx + lw, y, rw - lw - vw, S(26));
        put(app.detailVal, rx + rw - vw, y + S(4), vw, S(20));
        y += S(28);
        put(app.lblShade, rx, y + S(4), lw, S(20));
        put(app.shade, rx + lw, y, rw - lw - vw, S(26));
        put(app.shadeVal, rx + rw - vw, y + S(4), vw, S(20));
        y += S(30);
        put(app.lblBrush, rx, y + S(4), S(150), S(20));
        put(app.brush, rx + S(152), y, S(64), S(24));
        put(app.lblBrushPx, rx + S(222), y + S(4), S(30), S(20));
        y += S(28);
        put(app.invert, rx, y, rw / 2, S(22));
        put(app.stretch, rx + rw / 2, y, rw - rw / 2, S(22));
        y += S(26);
        app.sepY[0] = y;
        y += S(10);

        put(app.lblArea, rx, y, rw, S(18));
        y += S(20);
        put(app.pick, rx, y, rw - S(96), S(30));
        put(app.secs, rx + rw - S(88), y + S(3), S(58), S(24));
        put(app.lblSecs, rx + rw - S(26), y + S(7), S(26), S(20));
        y += S(34);
        put(app.areaInfo, rx, y, rw, S(20));
        y += S(22);
        int bw = (rw - S(6)) / 2;
        put(app.showArea, rx, y, bw, S(26));
        put(app.testBorder, rx + bw + S(6), y, rw - bw - S(6), S(26));
        y += S(32);
        app.sepY[1] = y;
        y += S(10);

        put(app.lblSpeed, rx, y, rw, S(18));
        y += S(20);
        put(app.speed, rx, y, rw, S(300));
        y += S(30);
        put(app.lblDelay, rx, y + S(4), S(54), S(20));
        put(app.delay, rx + S(56), y, S(60), S(24));
        put(app.lblDelayMs, rx + S(120), y + S(4), S(26), S(20));
        put(app.lblStep, rx + S(156), y + S(4), S(48), S(20));
        put(app.step, rx + S(206), y, S(60), S(24));
        put(app.lblStepPx, rx + S(270), y + S(4), S(24), S(20));
        y += S(28);
        put(app.limit, rx, y + S(1), S(118), S(22));
        put(app.limitEdit, rx + S(120), y, S(62), S(24));
        put(app.lblLimitS, rx + S(188), y + S(4), rw - S(188), S(20));
        y += S(28);
        put(app.failsafe, rx, y, rw, S(22));
        y += S(22);
        put(app.relative, rx, y, rw, S(22));
        y += S(28);

        const int drawH = S(46);
        int dy = std::max(y, bottom - pad - S(20) - drawH);
        put(app.draw, rx, dy, rw, drawH);
        put(app.hint, rx, dy + drawH + S(3), rw, S(18));
    }

    // Centre: preview + info line
    {
        const int cx = app.leftW, cw = W - app.leftW - app.rightW;
        put(app.preview, cx + 1, top, cw - 1, bottom - top - S(34));
        put(app.info, cx + pad, bottom - S(28), cw - 2 * pad, S(22));
    }
    EndDeferWindowPos(dw);
    // Up-down controls follow their buddies only when re-attached.
    for (auto [ud, buddy] : {std::pair{app.brushUd, app.brush}, std::pair{app.secsUd, app.secs},
                             std::pair{app.delayUd, app.delay}, std::pair{app.stepUd, app.step},
                             std::pair{app.limitUd, app.limitEdit}})
        SendMessageW(ud, UDM_SETBUDDY, reinterpret_cast<WPARAM>(buddy), 0);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void paintMain(HDC dc) {
    RECT rc;
    GetClientRect(app.hwnd, &rc);
    RECT sr{};
    if (app.status) GetWindowRect(app.status, &sr);
    const int bottom = rc.bottom - (sr.bottom - sr.top);
    const int top = headerHeight();

    RECT hdr{0, 0, rc.right, top};
    HBRUSH hb = CreateSolidBrush(kHeaderBg);
    FillRect(dc, &hdr, hb);
    DeleteObject(hb);
    RECT body{0, top, rc.right, rc.bottom};
    FillRect(dc, &body, app.brWhite);

    const int icon = S(32);
    if (app.headerIcon) DrawIconEx(dc, S(14), (top - icon) / 2, app.headerIcon, icon, icon, 0, nullptr, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, app.fontTitle);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT t{S(56), 0, rc.right / 2, top};
    DrawTextW(dc, L"Disegno", -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT calc = t;
    DrawTextW(dc, L"Disegno", -1, &calc, DT_LEFT | DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, app.font);
    SetTextColor(dc, kHeaderSub);
    RECT sub{calc.right + S(14), 0, rc.right - S(160), top};
    DrawTextW(dc, L"Dalla foto al disegno, col tuo mouse, in qualsiasi app", -1, &sub,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);

    HBRUSH sep = CreateSolidBrush(kSep);
    RECT l1{app.leftW, top, app.leftW + 1, bottom};
    RECT l2{rc.right - app.rightW, top, rc.right - app.rightW + 1, bottom};
    FillRect(dc, &l1, sep);
    FillRect(dc, &l2, sep);
    const int rx = rc.right - app.rightW + S(12), rw = app.rightW - S(24);
    for (int y : app.sepY) {
        RECT r{rx, y, rx + rw, y + 1};
        FillRect(dc, &r, sep);
    }
    DeleteObject(sep);
}

void drawOwnerButton(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    const bool pressed = di->itemState & ODS_SELECTED;
    const bool disabled = di->itemState & ODS_DISABLED;
    SetBkMode(dc, TRANSPARENT);
    if (di->CtlID == ID_HELP) {
        HBRUSH bg = CreateSolidBrush(kHeaderBg);
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        HBRUSH b = CreateSolidBrush(pressed ? kAccentDown : kAccent);
        HPEN p = CreatePen(PS_SOLID, 1, RGB(129, 140, 248));
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, S(12), S(12));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
        DeleteObject(p);
        HGDIOBJ of = SelectObject(dc, app.fontBold);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, L"?   Come si usa", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
        return;
    }
    // DISEGNA / FERMA
    FillRect(dc, &r, app.brWhite);
    const bool stop = app.phase != Phase::Idle;
    COLORREF c = stop ? (pressed ? kDangerDown : kDanger) : (pressed ? kAccentDown : kAccent);
    if (disabled) c = RGB(180, 182, 196);
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(14), S(14));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);

    const wchar_t* label = stop ? L"FERMA" : L"DISEGNA";
    HGDIOBJ of = SelectObject(dc, app.fontDraw);
    RECT tr = r;
    DrawTextW(dc, label, -1, &tr, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    const int tw = tr.right - tr.left, iconW = S(14), gap = S(10);
    const int x0 = r.left + (r.right - r.left - (iconW + gap + tw)) / 2;
    const int cy = (r.top + r.bottom) / 2;
    HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ owb = SelectObject(dc, wb), owp = SelectObject(dc, GetStockObject(NULL_PEN));
    if (stop) {
        Rectangle(dc, x0, cy - iconW / 2, x0 + iconW + 1, cy + iconW / 2 + 1);
    } else {
        POINT tri[3] = {{x0, cy - iconW / 2 - 1}, {x0 + iconW, cy}, {x0, cy + iconW / 2 + 1}};
        Polygon(dc, tri, 3);
    }
    SelectObject(dc, owb);
    SelectObject(dc, owp);
    DeleteObject(wb);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT txt{x0 + iconW + gap, r.top, r.right, r.bottom};
    DrawTextW(dc, label, -1, &txt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
    if (di->itemState & ODS_FOCUS) {
        RECT f = r;
        InflateRect(&f, -S(4), -S(4));
        DrawFocusRect(dc, &f);
    }
}

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

void createFonts() {
    for (HFONT f : {app.font, app.fontBold, app.fontSection, app.fontTitle, app.fontDraw})
        if (f) DeleteObject(f);
    app.font = wu::makeFont(app.dpi, 9);
    app.fontBold = wu::makeFont(app.dpi, 9, FW_SEMIBOLD);
    app.fontSection = wu::makeFont(app.dpi, 8, FW_BOLD);
    app.fontTitle = wu::makeFont(app.dpi, 15, FW_BOLD);
    app.fontDraw = wu::makeFont(app.dpi, 12, FW_BOLD);
    if (app.headerIcon) DestroyIcon(app.headerIcon);
    app.headerIcon = static_cast<HICON>(
        LoadImageW(app.inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, S(32), S(32), LR_DEFAULTCOLOR));
}

void applyFonts() {
    EnumChildWindows(
        app.hwnd,
        [](HWND h, LPARAM) -> BOOL {
            HFONT f = isSection(GetDlgCtrlID(h)) ? app.fontSection : app.font;
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
            return TRUE;
        },
        0);
}

void createControls() {
    app.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, app.hwnd, nullptr,
                                  app.inst, nullptr);
    SendMessageW(app.tooltip, TTM_SETMAXTIPWIDTH, 0, S(380));
    SendMessageW(app.tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);

    app.status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                                 app.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)), app.inst,
                                 nullptr);
    app.help = makeCtl(L"BUTTON", L"Come si usa", BS_OWNERDRAW | WS_TABSTOP, ID_HELP);

    // Library
    app.lblLibrary = makeLabel(L"LA TUA LIBRERIA", ID_LBL_LIBRARY);
    app.search = makeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_SEARCH, WS_EX_CLIENTEDGE);
    SendMessageW(app.search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Cerca una foto…"));
    app.btnOpen = makeCtl(L"BUTTON", L"Apri foto…", BS_PUSHBUTTON | WS_TABSTOP, ID_OPEN);
    app.btnPaste = makeCtl(L"BUTTON", L"Incolla", BS_PUSHBUTTON | WS_TABSTOP, ID_PASTE);
    app.list = makeCtl(WC_LISTVIEWW, L"",
                       WS_TABSTOP | LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_EDITLABELS | LVS_AUTOARRANGE,
                       ID_LIST);
    SetWindowTheme(app.list, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(app.list, LVS_EX_DOUBLEBUFFER);
    recreateThumbList();

    // Preview
    app.preview = preview::create(app.hwnd, ID_PREVIEW);
    app.info = makeLabel(L"", ID_INFO);

    // Style
    app.lblStyle = makeLabel(L"1   STILE DEL DISEGNO", ID_LBL_STYLE);
    app.style = makeCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, ID_STYLE);
    for (const wchar_t* n : kStyleNames) SendMessageW(app.style, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
    app.lblDetail = makeLabel(L"Dettaglio", ID_LBL_DETAIL);
    app.detail = makeCtl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, ID_DETAIL);
    SendMessageW(app.detail, TBM_SETRANGE, TRUE, MAKELPARAM(1, 10));
    app.detailVal = makeLabel(L"", ID_DETAIL_VAL, SS_RIGHT);
    app.lblShade = makeLabel(L"Ombre", ID_LBL_SHADE);
    app.shade = makeCtl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, ID_SHADE);
    SendMessageW(app.shade, TBM_SETRANGE, TRUE, MAKELPARAM(0, 10));
    app.shadeVal = makeLabel(L"", ID_SHADE_VAL, SS_RIGHT);
    app.lblBrush = makeLabel(L"Spessore pennello", ID_LBL_BRUSH);
    app.brush = makeCtl(L"EDIT", L"2", ES_NUMBER | WS_TABSTOP, ID_BRUSH, WS_EX_CLIENTEDGE);
    app.brushUd = makeUpDown(ID_BRUSH_UD, app.brush, 1, 40, 2);
    app.lblBrushPx = makeLabel(L"px", ID_LBL_BRUSH_PX);
    app.invert = makeCtl(L"BUTTON", L"Inverti colori", BS_AUTOCHECKBOX | WS_TABSTOP, ID_INVERT);
    app.stretch = makeCtl(L"BUTTON", L"Riempi l'area", BS_AUTOCHECKBOX | WS_TABSTOP, ID_STRETCH);

    // Area
    app.lblArea = makeLabel(L"2   DOVE DISEGNARE", ID_LBL_AREA);
    app.pick = makeCtl(L"BUTTON", L"Seleziona area", BS_PUSHBUTTON | WS_TABSTOP, ID_PICK);
    app.secs = makeCtl(L"EDIT", L"5", ES_NUMBER | WS_TABSTOP, ID_SECS, WS_EX_CLIENTEDGE);
    app.secsUd = makeUpDown(ID_SECS_UD, app.secs, 1, 30, 5);
    app.lblSecs = makeLabel(L"s", ID_LBL_SECS);
    app.areaInfo = makeLabel(L"", ID_AREA_INFO);
    app.showArea = makeCtl(L"BUTTON", L"Mostra area", BS_PUSHBUTTON | WS_TABSTOP, ID_SHOW_AREA);
    app.testBorder = makeCtl(L"BUTTON", L"Prova: disegna il bordo", BS_PUSHBUTTON | WS_TABSTOP, ID_TEST_BORDER);

    // Speed
    app.lblSpeed = makeLabel(L"3   VELOCITÀ", ID_LBL_SPEED);
    app.speed = makeCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, ID_SPEED);
    for (const SpeedPreset& sp : kSpeeds) SendMessageW(app.speed, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sp.name));
    app.lblDelay = makeLabel(L"Ritardo", ID_LBL_DELAY);
    app.delay = makeCtl(L"EDIT", L"", ES_NUMBER | WS_TABSTOP, ID_DELAY, WS_EX_CLIENTEDGE);
    app.delayUd = makeUpDown(ID_DELAY_UD, app.delay, 0, 500, 5);
    app.lblDelayMs = makeLabel(L"ms", ID_LBL_DELAY_MS);
    app.lblStep = makeLabel(L"Passo", ID_LBL_STEP);
    app.step = makeCtl(L"EDIT", L"", ES_NUMBER | WS_TABSTOP, ID_STEP, WS_EX_CLIENTEDGE);
    app.stepUd = makeUpDown(ID_STEP_UD, app.step, 1, 100, 6);
    app.lblStepPx = makeLabel(L"px", ID_LBL_STEP_PX);
    app.failsafe = makeCtl(L"BUTTON", L"Fermati se muovo il mouse (sicurezza)", BS_AUTOCHECKBOX | WS_TABSTOP,
                           ID_FAILSAFE);
    app.relative = makeCtl(L"BUTTON", L"Movimento relativo (per giochi difficili)", BS_AUTOCHECKBOX | WS_TABSTOP,
                           ID_RELATIVE);
    app.limit = makeCtl(L"BUTTON", L"Tempo massimo", BS_AUTOCHECKBOX | WS_TABSTOP, ID_LIMIT);
    app.limitEdit = makeCtl(L"EDIT", L"60", ES_NUMBER | WS_TABSTOP, ID_LIMIT_SECS, WS_EX_CLIENTEDGE);
    app.limitUd = makeUpDown(ID_LIMIT_UD, app.limitEdit, 5, 3600, 60);
    app.lblLimitS = makeLabel(L"s  (giochi a tempo)", ID_LBL_LIMIT_S);

    app.draw = makeCtl(L"BUTTON", L"DISEGNA", BS_OWNERDRAW | WS_TABSTOP, ID_DRAW);
    app.hint = makeLabel(L"ESC = ferma   ·   F8 = pausa / riprendi", ID_HINT, SS_CENTER);

    addTip(app.search, L"Cerca una foto della libreria per nome.");
    addTip(app.btnOpen, L"Aggiungi foto dal computer (Ctrl+O). Puoi sceglierne più di una.");
    addTip(app.btnPaste, L"Incolla un'immagine copiata, un file copiato o il link di un'immagine (Ctrl+V).");
    addTip(app.list, L"Tutte le foto che hai usato. Clicca per riusarla; tasto destro per rinominare, esportare "
                     L"o eliminare.");
    addTip(app.style, L"Contorni: solo le linee principali, il più veloce.\nSchizzo dettagliato: linee e ombre a "
                      L"tratteggio.\nTratteggio: solo ombre, come un'incisione.\nPuntini: puntini riga per riga, il "
                      L"più simile alla foto ma il più lento.");
    addTip(app.detail, L"Più dettaglio = più linee e più precisione, ma serve più tempo.");
    addTip(app.shade, L"Quanto sono ampie e scure le ombre (0 = nessuna ombra).");
    addTip(app.brush, L"Spessore della matita o del pennello nell'app dove disegni, in pixel. Con pennelli grossi "
                      L"il disegno usa meno linee e finisce prima.");
    addTip(app.invert, L"Per disegnare col bianco su un foglio scuro.");
    addTip(app.stretch, L"Riempie tutta l'area anche se la foto ha proporzioni diverse (la deforma).");
    addTip(app.pick, L"Hai i secondi indicati per portare il mouse nell'angolo IN ALTO A SINISTRA del foglio, poi "
                     L"altrettanti per l'angolo IN BASSO A DESTRA (F6).");
    addTip(app.secs, L"Secondi di attesa per ogni angolo e prima di iniziare a disegnare.");
    addTip(app.showArea, L"Mostra sullo schermo l'area scelta.");
    addTip(app.testBorder, L"Disegna solo il contorno dell'area: utile per controllare che sia quella giusta.");
    addTip(app.speed, L"«Automatica» riconosce l'app sotto l'area (Paint, browser, Roblox) e sceglie i tempi giusti.\n"
                      L"Roblox e molti giochi leggono il mouse una volta per fotogramma: se il tasto viene lasciato "
                      L"e ripremuto troppo in fretta non se ne accorgono e uniscono i tratti con delle righe. Con "
                      L"«Roblox e giochi» il tasto resta alzato abbastanza a lungo.");
    addTip(app.delay, L"Pausa dopo ogni movimento del mouse mentre disegna (solo «Personalizzata»).");
    addTip(app.step, L"Distanza massima tra due posizioni del mouse lungo una linea (solo «Personalizzata»).");
    addTip(app.failsafe, L"Se muovi il mouse mentre disegna, Disegno si ferma subito.");
    addTip(app.relative, L"Muove il mouse con spostamenti relativi, come un mouse vero. Prova se un gioco non "
                         L"disegna niente.");
    addTip(app.draw, L"Parte dopo il conto alla rovescia (F5). ESC per fermare, F8 per mettere in pausa.");
    addTip(app.limit, L"Se il disegno richiede più tempo, Disegno toglie i tratti meno importanti (prima i pezzettini "
                      L"e le ombre, poi il resto) così finisce entro i secondi indicati. Utile nei giochi a tempo.");
    addTip(app.limitEdit, L"Secondi a disposizione per il disegno.");
}

// ---------------------------------------------------------------------------
// Commands and notifications
// ---------------------------------------------------------------------------

void onCommand(int id, int code, HWND ctl) {
    (void)ctl;
    switch (id) {
        case ID_OPEN:
        case ID_PREVIEW:
            if (code == BN_CLICKED) openDialog();
            break;
        case ID_PASTE:
            pasteImage();
            break;
        case ID_HELP:
            showHelp();
            break;
        case ID_PICK:
            startPick();
            break;
        case ID_SHOW_AREA:
            if (app.hasArea) overlay::flashArea(app.area, 1600);
            break;
        case ID_TEST_BORDER:
            if (app.phase == Phase::Idle && app.hasArea) beginCountdown(true);
            break;
        case ID_DRAW:
            onDrawClicked();
            break;
        case ID_SEARCH:
            if (code == EN_CHANGE && !app.loadingUi) SetTimer(app.hwnd, TIMER_SEARCH, 250, nullptr);
            break;
        case ID_STYLE:
            if (code == CBN_SELCHANGE && !app.loadingUi) {
                app.params.style = dz::Style(std::clamp(int(SendMessageW(app.style, CB_GETCURSEL, 0, 0)), 0,
                                                        dz::kStyleCount - 1));
                updateValueLabels();
                saveSettingsLater();
                scheduleProcess(50);
            }
            break;
        case ID_SPEED:
            if (code == CBN_SELCHANGE && !app.loadingUi) {
                app.speedIndex = std::clamp(int(SendMessageW(app.speed, CB_GETCURSEL, 0, 0)), 0, kSpeedCount - 1);
                updateSpeedFields();
                if (app.limitOn) {
                    applyTimeLimit();
                    updatePreview();
                }
                updateInfo();
                saveSettingsLater();
            }
            break;
        case ID_BRUSH:
            if (code == EN_CHANGE && !app.loadingUi) {
                int v = std::clamp(getInt(app.brush, 2), 1, 40);
                if (float(v) != app.params.brush) {
                    app.params.brush = float(v);
                    saveSettingsLater();
                    scheduleProcess(350);
                }
            }
            break;
        case ID_SECS:
            if (code == EN_CHANGE && !app.loadingUi) {
                app.seconds = std::clamp(getInt(app.secs, 5), 1, 30);
                saveSettingsLater();
            }
            break;
        case ID_DELAY:
        case ID_STEP:
            if (code == EN_CHANGE && !app.loadingUi && app.speedIndex == kCustomSpeed) {
                app.custom.moveDelayMs = float(std::clamp(getInt(app.delay, 5), 0, 500));
                app.custom.stepPx = float(std::clamp(getInt(app.step, 6), 1, 100));
                if (app.limitOn) {
                    applyTimeLimit();
                    updatePreview();
                }
                updateInfo();
                saveSettingsLater();
            }
            break;
        case ID_INVERT:
            app.params.invert = Button_GetCheck(app.invert) == BST_CHECKED;
            saveSettingsLater();
            scheduleProcess(50);
            break;
        case ID_STRETCH:
            app.params.stretch = Button_GetCheck(app.stretch) == BST_CHECKED;
            saveSettingsLater();
            scheduleProcess(50);
            break;
        case ID_FAILSAFE:
            app.failsafeOn = Button_GetCheck(app.failsafe) == BST_CHECKED;
            saveSettingsLater();
            break;
        case ID_LIMIT:
            app.limitOn = Button_GetCheck(app.limit) == BST_CHECKED;
            EnableWindow(app.limitEdit, app.limitOn);
            EnableWindow(app.limitUd, app.limitOn);
            applyTimeLimit();
            updatePreview();
            updateInfo();
            saveSettingsLater();
            break;
        case ID_LIMIT_SECS:
            if (code == EN_CHANGE && !app.loadingUi) {
                app.limitSecs = std::clamp(getInt(app.limitEdit, 60), 5, 3600);
                if (app.limitOn) {
                    applyTimeLimit();
                    updatePreview();
                    updateInfo();
                }
                saveSettingsLater();
            }
            break;
        case ID_RELATIVE:
            app.relativeOn = Button_GetCheck(app.relative) == BST_CHECKED;
            saveSettingsLater();
            break;
        case IDM_RENAME: {
            int idx = selectedIndex();
            if (idx >= 0) {
                SetFocus(app.list);
                ListView_EditLabel(app.list, idx);
            }
            break;
        }
        case IDM_DELETE:
            deleteSelected();
            break;
        case IDM_EXPORT:
            exportSelected();
            break;
    }
}

LRESULT onNotify(NMHDR* nm) {
    if (nm->idFrom != ID_LIST) return 0;
    switch (nm->code) {
        case LVN_ITEMCHANGED: {
            auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
            if (app.loadingUi || !(lv->uChanged & LVIF_STATE)) break;
            if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) {
                int64_t id = int64_t(lv->lParam);
                if (id != app.currentId) loadFromLibrary(id);
            }
            break;
        }
        case NM_RCLICK: {
            if (selectedIndex() < 0) break;
            POINT pt;
            GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, IDM_RENAME, L"Rinomina\tF2");
            AppendMenuW(m, MF_STRING, IDM_EXPORT, L"Esporta immagine…");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, IDM_DELETE, L"Elimina\tCanc");
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, app.hwnd, nullptr);
            DestroyMenu(m);
            break;
        }
        case LVN_KEYDOWN: {
            auto* kd = reinterpret_cast<NMLVKEYDOWN*>(nm);
            if (kd->wVKey == VK_DELETE) deleteSelected();
            else if (kd->wVKey == VK_F2) onCommand(IDM_RENAME, 0, nullptr);
            break;
        }
        case LVN_BEGINLABELEDITW:
            return FALSE;
        case LVN_ENDLABELEDITW: {
            auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
            if (!di->item.pszText) return FALSE;
            std::wstring name = di->item.pszText;
            while (!name.empty() && iswspace(name.back())) name.pop_back();
            if (name.empty() || di->item.iItem < 0 || di->item.iItem >= int(app.items.size())) return FALSE;
            LibItem& it = app.items[size_t(di->item.iItem)];
            app.lib.rename(it.id, name);
            it.name = name;
            if (it.id == app.currentId) {
                app.currentName = name;
                SetWindowTextW(app.hwnd, (name + L"  –  Disegno").c_str());
            }
            return TRUE;
        }
        case LVN_GETEMPTYMARKUP: {
            auto* em = reinterpret_cast<NMLVEMPTYMARKUP*>(nm);
            em->dwFlags = EMF_CENTERED;
            wcsncpy(em->szMarkup,
                    getText(app.search).empty() ? L"La libreria è vuota.\nApri o incolla una foto."
                                                : L"Nessuna foto con questo nome.",
                    L_MAX_URL_LENGTH - 1);
            return TRUE;
        }
    }
    return 0;
}

bool handleShortcut(const MSG& m) {
    if (m.message != WM_KEYDOWN) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    wchar_t cls[32] = {};
    if (HWND f = GetFocus()) GetClassNameW(f, cls, 32);
    const bool inEdit = _wcsicmp(cls, L"Edit") == 0;
    if (ctrl && m.wParam == 'O') {
        openDialog();
        return true;
    }
    if (ctrl && m.wParam == 'V' && !inEdit) {
        pasteImage();
        return true;
    }
    if (m.wParam == VK_F5) {
        onDrawClicked();
        return true;
    }
    if (m.wParam == VK_F6) {
        startPick();
        return true;
    }
    if (m.wParam == VK_F1) {
        showHelp();
        return true;
    }
    return false;
}

LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            app.hwnd = hwnd;
            app.dpi = wu::dpiForWindow(hwnd);
            createFonts();
            createControls();
            applyFonts();
            DragAcceptFiles(hwnd, TRUE);
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = S(1000);
            mm->ptMinTrackSize.y = S(660);
            return 0;
        }
        case WM_DPICHANGED: {
            app.dpi = HIWORD(wp);
            createFonts();
            applyFonts();
            SendMessageW(app.tooltip, TTM_SETMAXTIPWIDTH, 0, S(380));
            recreateThumbList();
            refreshLibrary(app.currentId);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            paintMain(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            paintMain(reinterpret_cast<HDC>(wp));
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
            SetBkColor(dc, RGB(255, 255, 255));
            bool muted = isSection(id) || id == ID_HINT || id == ID_AREA_INFO || id == ID_INFO;
            SetTextColor(dc, muted ? kMuted : kText);
            return reinterpret_cast<LRESULT>(app.brWhite);
        }
        case WM_DRAWITEM:
            drawOwnerButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_HSCROLL: {
            HWND bar = reinterpret_cast<HWND>(lp);
            if (bar == app.detail || bar == app.shade) {
                int v = int(SendMessageW(bar, TBM_GETPOS, 0, 0));
                int& target = bar == app.detail ? app.params.detail : app.params.shading;
                if (v != target) {
                    target = v;
                    updateValueLabels();
                    saveSettingsLater();
                    scheduleProcess(250);
                }
            }
            return 0;
        }
        case WM_COMMAND:
            onCommand(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
            return 0;
        case WM_NOTIFY:
            return onNotify(reinterpret_cast<NMHDR*>(lp));
        case WM_TIMER:
            if (wp == TIMER_PROCESS) startProcess();
            else if (wp == TIMER_SAVE) saveSettingsNow();
            else if (wp == TIMER_SEARCH) {
                KillTimer(hwnd, TIMER_SEARCH);
                refreshLibrary(app.currentId);
            }
            return 0;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> files;
            for (UINT i = 0; i < n; ++i) {
                wchar_t path[MAX_PATH * 2];
                if (DragQueryFileW(drop, i, path, UINT(std::size(path)))) files.push_back(path);
            }
            DragFinish(drop);
            if (!files.empty()) openFiles(files);
            return 0;
        }
        case WM_APP_PROCESSED:
            onProcessed(uint64_t(wp), reinterpret_cast<dz::Drawing*>(lp));
            return 0;
        case WM_APP_AREA_PICKED:
            onAreaPicked(wp == 1, reinterpret_cast<RECT*>(lp));
            return 0;
        case WM_APP_COUNTDOWN_DONE:
            onCountdownDone(wp == 1);
            return 0;
        case WM_APP_DRAW_PROGRESS:
            onDrawProgress(int(wp), int(lp));
            return 0;
        case WM_APP_DRAW_PAUSED:
            onDrawPaused(wp != 0);
            return 0;
        case WM_APP_DRAW_DONE:
            onDrawDone(DrawResult(int(wp)), int(lp));
            return 0;
        case WM_CLOSE:
            // Let the drawing thread release the mouse button before the process exits.
            if (mouse::running()) {
                mouse::requestStop();
                for (int i = 0; i < 300 && mouse::running(); ++i) Sleep(10);
            }
            overlay::cancel();
            saveSettingsNow();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            hotkeys::disable();
            overlay::hideStatus(0);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int runMainWindow(HINSTANCE inst, int showCmd) {
    app.inst = inst;
    app.brWhite = CreateSolidBrush(RGB(255, 255, 255));

    std::wstring dbError;
    const std::wstring dbPath = wu::dataDir() + L"\\libreria.db";
    if (!app.lib.open(dbPath, &dbError)) {
        MessageBoxW(nullptr,
                    (L"Non riesco ad aprire la libreria delle foto:\n" + dbPath + L"\n\n" + dbError +
                     L"\n\nDisegno funziona lo stesso, ma le foto non verranno salvate.")
                        .c_str(),
                    kAppTitle, MB_OK | MB_ICONWARNING);
        app.lib.open(L":memory:", nullptr);
    }
    loadSettings();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = mainProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = kMainClass;
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, kMainClass, kAppTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1200, 800, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    // Size for the monitor the window landed on, or restore the last position.
    bool maximized = false;
    RECT saved{};
    int max = 0;
    const std::string ws = app.lib.setting("window");
    if (sscanf(ws.c_str(), "%ld,%ld,%ld,%ld,%d", &saved.left, &saved.top, &saved.right, &saved.bottom, &max) == 5 &&
        saved.right - saved.left > 200 && saved.bottom - saved.top > 200) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof wp;
        GetWindowPlacement(hwnd, &wp);
        wp.rcNormalPosition = saved;
        wp.showCmd = SW_HIDE;
        SetWindowPlacement(hwnd, &wp);
        maximized = max != 0;
    } else {
        app.dpi = wu::dpiForWindow(hwnd);
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        const RECT& wa = mi.rcWork;
        int w = std::min(S(1240), int(wa.right - wa.left)), h = std::min(S(820), int(wa.bottom - wa.top));
        SetWindowPos(hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w,
                     h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    detectTarget();
    syncControls();
    ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : showCmd);
    UpdateWindow(hwnd);

    // Photos passed on the command line ("Apri con…").
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> files;
    for (int i = 1; argv && i < argc; ++i)
        if (GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES) files.push_back(argv[i]);
    if (argv) LocalFree(argv);

    refreshLibrary(0);
    if (!files.empty()) {
        openFiles(files);
    } else {
        const int64_t last = _atoi64(app.lib.setting("last_image", "0").c_str());
        LibItem it;
        if (last && app.lib.item(last, it)) {
            refreshLibrary(last);
            loadFromLibrary(last);
        }
    }
    updateInfo();
    if (app.items.empty()) setStatus(L"Benvenuto! Apri o incolla una foto per iniziare (premi F1 per l'aiuto).");

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (handleShortcut(m)) continue;
        if (IsDialogMessageW(hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    app.lib.close();
    return int(m.wParam);
}
