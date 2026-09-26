// Disegno: draws any picture with the mouse, in any application.
#include "winutil.h"

#include <commctrl.h>
#include <objbase.h>

#include "mainwin.h"
#include "overlay.h"
#include "preview.h"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int showCmd) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES |
                                             ICC_UPDOWN_CLASS | ICC_BAR_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);
    overlay::registerClasses(inst);
    preview::registerClass(inst);
    int rc = runMainWindow(inst, showCmd);
    CoUninitialize();
    return rc;
}
