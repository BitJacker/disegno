#include "imageio.h"

#include <commdlg.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wininet.h>

#include <algorithm>
#include <cstring>
#include <cwctype>

#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

namespace imgio {

namespace {

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

int readOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q)) || !q) return 1;
    const wchar_t* names[] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"System.Photo.Orientation"};
    for (const wchar_t* n : names) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(n, &v))) {
            int o = 1;
            if (v.vt == VT_UI2) o = v.uiVal;
            else if (v.vt == VT_UI4) o = int(v.ulVal);
            else if (v.vt == VT_I4) o = v.lVal;
            PropVariantClear(&v);
            if (o >= 1 && o <= 8) return o;
        }
    }
    return 1;
}

bool decodeWic(const Bytes& data, dz::Rgba& out, int maxSide) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                reinterpret_cast<void**>(&f.p))))
        return false;
    ComPtr<IWICStream> stream;
    if (FAILED(f->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(data.data())),
                                            DWORD(data.size()))))
        return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || !w || !h) return false;
    const int orientation = readOrientation(frame.p);

    IWICBitmapSource* src = frame.p;
    ComPtr<IWICBitmapScaler> scaler;
    if (int(std::max(w, h)) > maxSide) {
        double s = double(maxSide) / double(std::max(w, h));
        UINT nw = std::max(1u, UINT(w * s + 0.5)), nh = std::max(1u, UINT(h * s + 0.5));
        if (SUCCEEDED(f->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(src, nw, nh, WICBitmapInterpolationModeFant))) {
            src = scaler.p;
            w = nw;
            h = nh;
        }
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeMedianCut)))
        return false;
    dz::Rgba img{int(w), int(h)};
    if (FAILED(conv->CopyPixels(nullptr, w * 4, w * h * 4, img.p.data()))) return false;
    for (size_t i = 0; i < img.p.size(); i += 4) std::swap(img.p[i], img.p[i + 2]);  // BGRA -> RGBA
    out = dz::applyExifOrientation(img, orientation);
    return true;
}

bool decodeStb(const Bytes& data, dz::Rgba& out, int maxSide) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data.data()), int(data.size()), &w,
                                              &h, &n, 4);
    if (!px) return false;
    dz::Rgba img(w, h);
    std::memcpy(img.p.data(), px, size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    if (std::max(w, h) > maxSide) {
        double s = double(maxSide) / std::max(w, h);
        img = dz::resize(img, std::max(1, int(w * s + 0.5)), std::max(1, int(h * s + 0.5)));
    }
    out = std::move(img);
    return true;
}

void appendBytes(void* ctx, void* data, int size) {
    static_cast<Bytes*>(ctx)->append(static_cast<const char*>(data), size_t(size));
}

bool startsWith(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (towlower(s[i]) != towlower(prefix[i])) return false;
    return true;
}

std::wstring trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (iswspace(s[a]) || s[a] == L'"')) ++a;
    while (b > a && (iswspace(s[b - 1]) || s[b - 1] == L'"')) --b;
    return s.substr(a, b - a);
}

bool base64Decode(const std::string& in, Bytes& out) {
    static int table[256];
    static bool init = false;
    if (!init) {
        for (int& t : table) t = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(a[i])] = i;
        table[static_cast<unsigned char>('-')] = 62;
        table[static_cast<unsigned char>('_')] = 63;
        init = true;
    }
    out.clear();
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int d = table[c];
        if (d < 0) continue;  // skip whitespace / line breaks
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(char((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return !out.empty();
}

bool bitmapToRgba(HBITMAP hb, dz::Rgba& out) {
    BITMAP bm{};
    if (!GetObjectW(hb, sizeof bm, &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) return false;
    const int w = bm.bmWidth, h = std::abs(bm.bmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dz::Rgba img(w, h);
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, hb, 0, UINT(h), img.p.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines <= 0) return false;
    bool anyAlpha = false;
    for (size_t i = 0; i < img.p.size(); i += 4) {
        std::swap(img.p[i], img.p[i + 2]);
        if (img.p[i + 3]) anyAlpha = true;
    }
    if (!anyAlpha)
        for (size_t i = 3; i < img.p.size(); i += 4) img.p[i] = 255;
    out = std::move(img);
    return true;
}

}  // namespace

bool decode(const Bytes& data, dz::Rgba& out, int maxSide) {
    if (data.empty()) return false;
    return decodeWic(data, out, maxSide) || decodeStb(data, out, maxSide);
}

bool encodePng(const dz::Rgba& img, Bytes& out) {
    out.clear();
    if (img.empty()) return false;
    return stbi_write_png_to_func(appendBytes, &out, img.w, img.h, 4, img.p.data(), img.w * 4) != 0;
}

Bytes makeThumbnail(const dz::Rgba& img, int size) {
    if (img.empty()) return {};
    double s = std::min(1.0, double(size) / double(std::max(img.w, img.h)));
    dz::Rgba t = dz::resize(img, std::max(1, int(img.w * s + 0.5)), std::max(1, int(img.h * s + 0.5)));
    Bytes png;
    encodePng(t, png);
    return png;
}

bool isImagePath(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot + 1);
    for (auto& c : ext) c = wchar_t(towlower(c));
    static const wchar_t* exts[] = {L"jpg", L"jpeg", L"jfif", L"png", L"bmp", L"dib",  L"gif",  L"webp", L"tif",
                                    L"tiff", L"heic", L"heif", L"avif", L"ico", L"jxr", L"wdp", L"tga", L"psd"};
    for (const wchar_t* e : exts)
        if (ext == e) return true;
    return false;
}

bool download(const std::wstring& urlIn, Bytes& data, std::wstring* error) {
    std::wstring url = trim(urlIn);
    data.clear();
    if (startsWith(url, L"data:")) {
        size_t comma = url.find(L',');
        if (comma == std::wstring::npos || url.find(L";base64") == std::wstring::npos) {
            if (error) *error = L"Formato del link non supportato.";
            return false;
        }
        if (!base64Decode(wu::narrow(url.substr(comma + 1)), data)) {
            if (error) *error = L"Dati dell'immagine non validi.";
            return false;
        }
        return true;
    }
    if (!startsWith(url, L"http://") && !startsWith(url, L"https://")) {
        if (error) *error = L"Il link deve iniziare con http:// o https://";
        return false;
    }
    HINTERNET inet = InternetOpenW(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) Disegno/1.0",
                                   INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!inet) {
        if (error) *error = L"Connessione a Internet non disponibile.";
        return false;
    }
    HINTERNET h = InternetOpenUrlW(inet, url.c_str(), L"Accept: image/*,*/*;q=0.8\r\n", DWORD(-1),
                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!h) {
        InternetCloseHandle(inet);
        if (error) *error = L"Impossibile aprire il link.";
        return false;
    }
    DWORD status = 0, len = sizeof status;
    if (HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr) && status >= 400) {
        InternetCloseHandle(h);
        InternetCloseHandle(inet);
        if (error) *error = L"Il sito ha risposto con errore " + std::to_wstring(status) + L".";
        return false;
    }
    char buf[64 * 1024];
    DWORD got = 0;
    bool ok = true;
    while (InternetReadFile(h, buf, sizeof buf, &got) && got > 0) {
        data.append(buf, got);
        if (data.size() > 80u * 1024u * 1024u) {
            ok = false;
            if (error) *error = L"Il file è troppo grande.";
            break;
        }
    }
    InternetCloseHandle(h);
    InternetCloseHandle(inet);
    if (ok && data.empty()) {
        ok = false;
        if (error) *error = L"Il link non contiene dati.";
    }
    return ok;
}

bool fromClipboard(HWND owner, Bytes& data, std::wstring& name, std::wstring& source, std::wstring* error) {
    data.clear();
    source.clear();
    if (!OpenClipboard(owner)) {
        if (error) *error = L"Gli appunti sono occupati da un'altra applicazione. Riprova.";
        return false;
    }
    bool ok = false;
    std::wstring text;
    const UINT cfPng = RegisterClipboardFormatW(L"PNG");
    if (cfPng && IsClipboardFormatAvailable(cfPng)) {
        if (HANDLE hmem = GetClipboardData(cfPng)) {
            if (const void* p = GlobalLock(hmem)) {
                data.assign(static_cast<const char*>(p), GlobalSize(hmem));
                GlobalUnlock(hmem);
                ok = !data.empty();
                name = L"Incollata " + wu::nowStamp();
            }
        }
    }
    if (!ok && IsClipboardFormatAvailable(CF_HDROP)) {
        if (HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count && !ok; ++i) {
                wchar_t path[MAX_PATH * 2];
                if (!DragQueryFileW(drop, i, path, UINT(std::size(path)))) continue;
                if (isImagePath(path) && wu::readFile(path, data)) {
                    ok = true;
                    name = wu::fileStem(path);
                    source = path;
                }
            }
        }
    }
    if (!ok && IsClipboardFormatAvailable(CF_BITMAP)) {
        if (HBITMAP hb = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP))) {
            dz::Rgba img;
            if (bitmapToRgba(hb, img) && encodePng(img, data)) {
                ok = true;
                name = L"Incollata " + wu::nowStamp();
            }
        }
    }
    if (!ok && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (HANDLE hmem = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(hmem))) {
                text = trim(p);
                GlobalUnlock(hmem);
            }
        }
    }
    CloseClipboard();
    if (ok) return true;

    if (!text.empty()) {
        if (startsWith(text, L"http://") || startsWith(text, L"https://") || startsWith(text, L"data:")) {
            if (!download(text, data, error)) return false;
            source = startsWith(text, L"data:") ? L"" : text;
            std::wstring stem = startsWith(text, L"data:") ? L"" : wu::fileStem(text.substr(0, text.find(L'?')));
            name = stem.empty() || stem.size() > 60 ? L"Da Internet " + wu::nowStamp() : stem;
            return true;
        }
        if (GetFileAttributesW(text.c_str()) != INVALID_FILE_ATTRIBUTES && wu::readFile(text, data)) {
            name = wu::fileStem(text);
            source = text;
            return true;
        }
    }
    if (error)
        *error = L"Negli appunti non c'è un'immagine.\nCopia una foto (tasto destro → Copia immagine) "
                 L"oppure il link di un'immagine, poi premi di nuovo Incolla.";
    return false;
}

std::vector<std::wstring> pickImages(HWND owner) {
    std::vector<wchar_t> buf(64 * 1024, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter =
        L"Immagini\0*.jpg;*.jpeg;*.jfif;*.png;*.bmp;*.gif;*.webp;*.tif;*.tiff;*.heic;*.heif;*.avif;*.ico;*.tga;*.psd\0"
        L"Tutti i file\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = DWORD(buf.size());
    ofn.lpstrTitle = L"Scegli una o più immagini";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
    std::vector<std::wstring> out;
    if (!GetOpenFileNameW(&ofn)) return out;
    const wchar_t* p = buf.data();
    std::wstring first = p;
    p += first.size() + 1;
    if (!*p) {
        out.push_back(first);  // single file: full path
        return out;
    }
    while (*p) {
        std::wstring name = p;
        out.push_back(first + L"\\" + name);
        p += name.size() + 1;
    }
    return out;
}

bool pickSavePath(HWND owner, const std::wstring& suggestedName, std::wstring& path) {
    std::vector<wchar_t> buf(MAX_PATH * 4, L'\0');
    std::wstring name = suggestedName;
    for (auto& c : name)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    wcsncpy(buf.data(), name.c_str(), buf.size() - 1);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Tutti i file\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = DWORD(buf.size());
    ofn.lpstrTitle = L"Esporta immagine";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return false;
    path = buf.data();
    return true;
}

}  // namespace imgio
