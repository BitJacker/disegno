// Preview panel: shows the drawing that will be made (or the photo) on a sheet with
// the proportions of the selected area, and can animate the stroke order.
#pragma once

#include <memory>
#include <string>

#include "../core/image.h"
#include "../core/strokes.h"
#include "winutil.h"

namespace preview {

struct Content {
    std::shared_ptr<const dz::Drawing> drawing;
    std::shared_ptr<const dz::Rgba> photo;
    float areaW = 800;
    float areaH = 600;
    bool stretch = false;
    bool busy = false;
};

bool registerClass(HINSTANCE inst);
HWND create(HWND parent, int id);
void setContent(HWND hwnd, const Content& c);
void setBusy(HWND hwnd, bool busy);
void showPhoto(HWND hwnd, bool photo);

}  // namespace preview
