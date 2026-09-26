// Image loading/saving, clipboard, downloads and file dialogs.
#pragma once

#include <string>
#include <vector>

#include "../core/image.h"
#include "winutil.h"

namespace imgio {

// Decodes with Windows Imaging Component (JPEG, PNG, BMP, GIF, TIFF, WebP/HEIC when the codec is
// installed...) falling back to stb_image. Applies the EXIF rotation. Images whose long side is
// larger than maxSide are scaled down while decoding.
bool decode(const Bytes& data, dz::Rgba& out, int maxSide = 4096);

bool encodePng(const dz::Rgba& img, Bytes& out);

// PNG thumbnail that fits in size x size (aspect ratio kept).
Bytes makeThumbnail(const dz::Rgba& img, int size);

// Reads an image from the clipboard: PNG data, copied files, bitmaps, or a copied
// link / file path. `name` gets a suggested library name.
bool fromClipboard(HWND owner, Bytes& data, std::wstring& name, std::wstring& source, std::wstring* error);

// Downloads an http(s) link or decodes a data: URL.
bool download(const std::wstring& url, Bytes& data, std::wstring* error);

std::vector<std::wstring> pickImages(HWND owner);
bool pickSavePath(HWND owner, const std::wstring& suggestedName, std::wstring& path);

bool isImagePath(const std::wstring& path);

}  // namespace imgio
