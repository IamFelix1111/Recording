// State-colored window icons and the antialiased status dot.
// Extracted from gui.cpp: rendering only, no window lifecycle.

#include <windows.h>
#include <shobjidl.h>

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "gui_internal.h"

using Microsoft::WRL::ComPtr;

// Single source of truth for the state color (status dot + window/tray icon).
COLORREF stateColor(UiState st) {
    switch (st) {
        case UiState::Ready: return RGB(46, 160, 67);     // green
        case UiState::Recording: return RGB(200, 30, 30); // red
        case UiState::Paused: return RGB(210, 150, 0);    // yellow
        default: return RGB(0, 120, 215);                 // Preparing / Saving: blue
    }
}

// Antialiased dot: per-pixel analytic circle coverage (4x4 supersampling),
// blended over the window background. GDI's Ellipse is aliased and, at
// ~8px under 125% DPI, visibly lumpy - this renders a true smooth circle.
void drawDotAntialiased(HDC dc, const RECT& rc, COLORREF c) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const double cx = w / 2.0, cy = h / 2.0;
    const double r = (std::min)(w, h) / 2.0 - 0.25;
    const COLORREF bg = GetSysColor(COLOR_WINDOW);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int inside = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const double dx = x + (sx + 0.5) / 4.0 - cx;
                    const double dy = y + (sy + 0.5) / 4.0 - cy;
                    if (dx * dx + dy * dy <= r * r) ++inside;
                }
            }
            const double a = inside / 16.0;
            const COLORREF px = RGB(
                static_cast<int>(GetRValue(c) * a + GetRValue(bg) * (1 - a)),
                static_cast<int>(GetGValue(c) * a + GetGValue(bg) * (1 - a)),
                static_cast<int>(GetBValue(c) * a + GetBValue(bg) * (1 - a)));
            SetPixel(dc, rc.left + x, rc.top + y, px);
        }
    }
}

// Renders the app icon (dark monitor + colored record dot) at the requested
// size with 3x3 supersampled edges, same design language as icon.svg.
static HICON makeStateIcon(int size, COLORREF dot) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = size;
    bi.bV5Height = -size;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(sdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, sdc);
    if (!color || !bits) return nullptr;

    // Signed-distance helpers in normalized [0,1] icon space.
    auto sdRect = [](double x, double y, double cx, double cy, double hw, double hh,
                     double r) {
        const double qx = std::fabs(x - cx) - (hw - r);
        const double qy = std::fabs(y - cy) - (hh - r);
        const double ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
        return std::sqrt(ax * ax + ay * ay) + std::min(std::max(qx, qy), 0.0) - r;
    };
    constexpr COLORREF bezel = RGB(89, 99, 122);
    constexpr COLORREF screen = RGB(35, 40, 51);
    auto* px = static_cast<std::uint32_t*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int inBezel = 0, inScreen = 0, inDot = 0;
            for (int sy = 0; sy < 3; ++sy) {
                for (int sx = 0; sx < 3; ++sx) {
                    const double u = (x + (sx + 0.5) / 3.0) / size;
                    const double v = (y + (sy + 0.5) / 3.0) / size;
                    if (sdRect(u, v, 0.5, 0.47, 0.45, 0.37, 0.10) < 0) {
                        ++inBezel;
                        if (sdRect(u, v, 0.5, 0.46, 0.38, 0.28, 0.05) < 0) {
                            ++inScreen;
                            const double dx = u - 0.5, dy = v - 0.46;
                            if (dx * dx + dy * dy <= 0.15 * 0.15) ++inDot;
                        }
                    }
                }
            }
            const double aBezel = inBezel / 9.0;
            const double aScreen = inScreen / 9.0;
            const double aDot = inDot / 9.0;
            // Compose screen over bezel, dot over screen.
            auto mix = [](COLORREF over, double aOver, COLORREF under) -> COLORREF {
                return RGB(
                    static_cast<int>(GetRValue(over) * aOver +
                                     GetRValue(under) * (1 - aOver)),
                    static_cast<int>(GetGValue(over) * aOver +
                                     GetGValue(under) * (1 - aOver)),
                    static_cast<int>(GetBValue(over) * aOver +
                                     GetBValue(under) * (1 - aOver)));
            };
            const COLORREF base = mix(screen, aScreen, bezel);
            const COLORREF rgb = mix(dot, aDot, base);
            px[static_cast<size_t>(y) * size + x] =
                (static_cast<std::uint32_t>(std::lround(aBezel * 255.0)) << 24) |
                (static_cast<std::uint32_t>(GetRValue(rgb)) << 16) |
                (static_cast<std::uint32_t>(GetGValue(rgb)) << 8) |
                static_cast<std::uint32_t>(GetBValue(rgb));
        }
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// Renders an antialiased dot with a thin gray ring on transparency - the
// taskbar overlay badge. The ring keeps the badge readable on top of the
// red dot in the main icon. Dot ~60% of the canvas, ring ~1px+.
static HICON makeDotIcon(int size, COLORREF c) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = size;
    bi.bV5Height = -size;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(sdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, sdc);
    if (!color || !bits) return nullptr;

    const double rIn = size * 0.30;                       // state-color dot
    const double rOut = rIn + (std::max)(1.0, size * 0.08);  // gray ring
    const double cx = size / 2.0, cy = size / 2.0;
    const COLORREF ring = RGB(160, 160, 160);  // gray ring (subtle)
    auto* px = static_cast<std::uint32_t*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int inDot = 0, inRing = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const double dx = x + (sx + 0.5) / 4.0 - cx;
                    const double dy = y + (sy + 0.5) / 4.0 - cy;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 <= rIn * rIn)
                        ++inDot;
                    else if (d2 <= rOut * rOut)
                        ++inRing;
                }
            }
            const int covered = inDot + inRing;
            const double a = covered / 16.0;
            int r = 0, g = 0, b = 0;
            if (covered > 0) {
                r = (GetRValue(c) * inDot + GetRValue(ring) * inRing) / covered;
                g = (GetGValue(c) * inDot + GetGValue(ring) * inRing) / covered;
                b = (GetBValue(c) * inDot + GetBValue(ring) * inRing) / covered;
            }
            px[static_cast<size_t>(y) * size + x] =
                (static_cast<std::uint32_t>(std::lround(a * 255.0)) << 24) |
                (static_cast<std::uint32_t>(r) << 16) |
                (static_cast<std::uint32_t>(g) << 8) |
                static_cast<std::uint32_t>(b);
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// State -> icon cache bucket. Must stay in sync with stateColor().
// Bucket 4 (dim red) is only used by the tray blink, never the main icon.
int stateIconBucket(UiState st) {
    switch (st) {
        case UiState::Ready: return 0;      // green
        case UiState::Recording: return 1;  // red
        case UiState::Paused: return 2;     // yellow
        default: return 3;                  // Preparing / Saving: blue
    }
}

// Pre-generates all state icons once (buckets x big/small).
void ensureStateIcons() {
    static constexpr COLORREF colors[kIconBucketCount] = {
        RGB(46, 160, 67),   // green
        RGB(200, 30, 30),   // red
        RGB(210, 150, 0),   // yellow
        RGB(0, 120, 215),   // blue
        RGB(110, 16, 16),   // dim red (tray blink, "off" phase)
    };
    for (int i = 0; i < kIconBucketCount; ++i) {
        if (!g.stateIcons[i][0])
            g.stateIcons[i][0] =
                makeStateIcon(GetSystemMetrics(SM_CXICON), colors[i]);
        if (!g.stateIcons[i][1])
            g.stateIcons[i][1] =
                makeStateIcon(GetSystemMetrics(SM_CXSMICON), colors[i]);
    }
}

void destroyStateIcons() {
    for (auto& pair : g.stateIcons) {
        for (HICON& h : pair) {
            if (h) DestroyIcon(h);
            h = nullptr;
        }
    }
    if (g.overlayDot) {
        DestroyIcon(g.overlayDot);
        g.overlayDot = nullptr;
    }
    if (g.overlayBlank) {
        DestroyIcon(g.overlayBlank);
        g.overlayBlank = nullptr;
    }
}

// --- taskbar overlay badge (ITaskbarList3, Win7+) ---------------------------

static ComPtr<ITaskbarList3> g_taskbarList;

static ComPtr<ITaskbarList3> taskbarList() {
    if (!g_taskbarList) {
        if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&g_taskbarList))))
            g_taskbarList.Reset();
    }
    return g_taskbarList;
}

// Release the COM reference before CoUninitialize; an STA object outliving
// it delays process teardown by a couple of seconds.
void releaseTaskbarCom() { g_taskbarList.Reset(); }

void clearTaskbarOverlay() {
    if (auto list = taskbarList()) list->SetOverlayIcon(g.wnd, nullptr, nullptr);
}

static void ensureOverlayIcons() {
    const int sz = GetSystemMetrics(SM_CXSMICON);
    if (!g.overlayDot) g.overlayDot = makeDotIcon(sz, RGB(200, 30, 30));
    // "Off" phase: dark red, not transparent (a fully transparent overlay
    // renders black on some taskbars). Icon-for-icon swaps stay immediate.
    if (!g.overlayBlank) g.overlayBlank = makeDotIcon(sz, RGB(110, 16, 16));
}

// Swaps the window (title bar / taskbar / Alt-Tab) icons to the state color.
// Icons are pre-generated per color bucket and reused - switching is instant.
void updateStateIcons() {
    if (!g.wnd) return;
    ensureStateIcons();
    const int b = stateIconBucket(g.state);
    if (g.stateIcons[b][0])
        SendMessageW(g.wnd, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(g.stateIcons[b][0]));
    if (g.stateIcons[b][1])
        SendMessageW(g.wnd, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(g.stateIcons[b][1]));
    if (g.state != UiState::Recording) clearTaskbarOverlay();
}

// Applies the current blink phase to overlay + tray (no state check).
static void applyBlinkPhase() {
    ensureOverlayIcons();
    // Icon-for-icon swap (red dot <-> transparent): immediate, without the
    // fade the taskbar plays when an overlay is added or removed.
    if (auto list = taskbarList())
        list->SetOverlayIcon(g.wnd, g.blinkOn ? g.overlayDot : g.overlayBlank,
                             L"REC");
    traySetBlinkPhase(g.blinkOn);
}

// Entering the recording state: badge on, tray icon red.
void blinkRecordingStart() {
    g.blinkOn = true;
    applyBlinkPhase();
}

// Called from the 200 ms poll: while recording, blinks the taskbar overlay
// badge (red dot <-> none) and mirrors the phase onto the tray icon.
void onBlinkTick() {
    if (!g.wnd || g.state != UiState::Recording) return;
    const bool phase = (GetTickCount64() / 1200) % 2 == 0;
    if (phase == g.blinkOn) return;
    g.blinkOn = phase;
    applyBlinkPhase();
}
