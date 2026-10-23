// Red border frame around the recorded region: four strip windows
// (top/bottom/left/right), each topmost, click-through and excluded from
// screen capture (WDA_EXCLUDEFROMCAPTURE). No single window spans the whole
// monitor - exact fullscreen bounds would trigger the shell's fullscreen
// heuristic and auto-hide the taskbar.

#include <windows.h>

#include <algorithm>

#include "gui_internal.h"

HWND g_borderWnds[4] = {};  // top, bottom, left, right

namespace {

LRESULT CALLBACK borderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            const bool paused = g.sess && g.sess->isPaused();
            HBRUSH br = CreateSolidBrush(paused ? RGB(230, 170, 0) : RGB(220, 30, 30));
            FillRect(dc, &rc, br);
            DeleteObject(br);
            EndPaint(h, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(h, msg, wp, lp);
    }
}

}  // namespace

void registerBorderClass(HINSTANCE inst) {
    WNDCLASSW wb{};
    wb.lpfnWndProc = borderProc;
    wb.hInstance = inst;
    wb.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wb.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    wb.lpszClassName = L"RecordingBorder";
    RegisterClassW(&wb);
}

void destroyBorderWindow() {
    for (HWND& h : g_borderWnds) {
        if (h) {
            DestroyWindow(h);
            h = nullptr;
        }
    }
}

void createBorderWindow() {
    destroyBorderWindow();
    if (!g.sess) return;
    const int x = g.sess->capX.load(), y = g.sess->capY.load();
    const int w = g.sess->capW.load(), h = g.sess->capH.load();
    if (w <= 0 || h <= 0) return;
    int bw = S(5);
    if (bw * 6 > w || bw * 6 > h) bw = (std::min)(w, h) / 6;  // tiny regions
    if (bw < 1) bw = 1;

    // Strip layout: top/bottom span the full width, left/right fill between.
    // Sides may vanish on extremely flat regions; that is fine.
    const RECT strips[4] = {
        {x, y, x + w, y + bw},                       // top
        {x, y + h - bw, x + w, y + h},               // bottom (pixel-exact)
        {x, y + bw, x + bw, y + h - bw},             // left
        {x + w - bw, y + bw, x + w, y + h - bw},     // right
    };
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    for (int i = 0; i < 4; ++i) {
        if (strips[i].right <= strips[i].left || strips[i].bottom <= strips[i].top)
            continue;
        g_borderWnds[i] = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE |
                WS_EX_TOOLWINDOW,
            L"RecordingBorder", nullptr, WS_POPUP, strips[i].left, strips[i].top,
            strips[i].right - strips[i].left, strips[i].bottom - strips[i].top,
            nullptr, nullptr, inst, nullptr);
        if (!g_borderWnds[i]) continue;
        SetLayeredWindowAttributes(g_borderWnds[i], 0, 255, LWA_ALPHA);
        // Keep the strip out of the captured video (Windows 10 2004+).
        SetWindowDisplayAffinity(g_borderWnds[i], WDA_EXCLUDEFROMCAPTURE);
        ShowWindow(g_borderWnds[i], SW_SHOWNOACTIVATE);
    }
    g.borderPaused = g.sess->isPaused();
}

void updateBorderWindow() {
    if (!g.sess) return;
    const bool anyUp = g_borderWnds[0] || g_borderWnds[1] || g_borderWnds[2] ||
                       g_borderWnds[3];
    if (!anyUp) return;
    // Reassert topmost in case another topmost window covered the strips.
    for (HWND h : g_borderWnds) {
        if (h)
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE |
                             SWP_NOOWNERZORDER);
    }
    const bool paused = g.sess->isPaused();
    if (paused != g.borderPaused) {
        g.borderPaused = paused;
        for (HWND h : g_borderWnds) {
            if (h) InvalidateRect(h, nullptr, FALSE);
        }
    }
}
