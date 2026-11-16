// Prism Pet - the pet window's message handling.
#include "pet_internal.h"

namespace pet {

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // never takes focus
    case WM_DISPLAYCHANGE: case WM_DPICHANGED: Relayout(); return 0;
    case WM_SETTINGCHANGE:   // taskbar moved, resized or set to auto-hide
        if (wp == SPI_SETWORKAREA) Relayout();
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace pet
