#include "winutil.h"

#include <shlobj.h>

#include <cmath>
#include <cwchar>

namespace wu {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

UINT systemDpi() {
    HDC dc = GetDC(nullptr);
    UINT dpi = dc ? UINT(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}

UINT dpiForWindow(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d) return d;
    }
    return systemDpi();
}

UINT dpiForPoint(POINT pt) {
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static Fn fn = [] {
        HMODULE m = LoadLibraryW(L"shcore.dll");
        return m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "GetDpiForMonitor"))) : nullptr;
    }();
    if (fn) {
        UINT x = 0, y = 0;
        if (SUCCEEDED(fn(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), 0 /*MDT_EFFECTIVE_DPI*/, &x, &y)) && x)
            return x;
    }
    return systemDpi();
}

bool adjustWindowRectForDpi(RECT* rc, DWORD style, DWORD exStyle, UINT dpi) {
    using Fn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static Fn fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi")));
    if (fn) return fn(rc, style, FALSE, exStyle, dpi) != FALSE;
    return AdjustWindowRectEx(rc, style, FALSE, exStyle) != FALSE;
}

HFONT makeFont(UINT dpi, int pointSize, int weight, const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(pointSize, int(dpi), 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

std::wstring exeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::wstring dataDir() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    std::wstring exe = exeDir();
    if (GetFileAttributesW((exe + L"\\disegno-portable.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
        cached = exe;
        return cached;
    }
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base) {
        dir = std::wstring(base) + L"\\Disegno";
        CoTaskMemFree(base);
    } else {
        dir = exe;
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    cached = dir;
    return cached;
}

std::wstring formatDuration(double s) {
    if (!(s >= 0)) s = 0;
    long long total = llround(s);
    wchar_t buf[64];
    if (total < 60) swprintf(buf, 64, L"%lld s", total < 1 ? 1LL : total);
    else if (total < 3600) swprintf(buf, 64, L"%lld min %02lld s", total / 60, total % 60);
    else swprintf(buf, 64, L"%lld h %02lld min", total / 3600, (total % 3600) / 60);
    return buf;
}

std::wstring formatInt(long long n) {
    std::wstring digits = std::to_wstring(n < 0 ? -n : n);
    std::wstring out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count && count % 3 == 0) out.insert(out.begin(), L'.');
        out.insert(out.begin(), *it);
        ++count;
    }
    if (n < 0) out.insert(out.begin(), L'-');
    return out;
}

std::wstring fileStem(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

std::wstring nowStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%02u/%02u/%04u %02u:%02u", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
    return buf;
}

bool readFile(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < (1LL << 31);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = size.QuadPart == 0 || (ReadFile(f, out.data(), DWORD(size.QuadPart), &read, nullptr) &&
                                    read == DWORD(size.QuadPart));
    }
    CloseHandle(f);
    return ok;
}

bool writeFile(const std::wstring& path, const void* data, size_t size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, data, DWORD(size), &written, nullptr) && written == DWORD(size);
    CloseHandle(f);
    return ok;
}

std::vector<uint32_t> captureScreen(const RECT& r) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    std::vector<uint32_t> px;
    if (w <= 0 || h <= 0) return px;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        if (BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT)) {
            GdiFlush();
            const auto* p = static_cast<const uint32_t*>(bits);
            px.assign(p, p + size_t(w) * size_t(h));
        }
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return px;
}

}  // namespace wu
