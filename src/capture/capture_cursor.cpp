// Cursor overlay onto a raw BGRA buffer (the DXGI backend composites the
// mouse pointer per frame; the GDI backend uses DrawIconEx into its DC).

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "capture_internal.h"

std::string hrText(HRESULT hr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

namespace {

struct IconImages {
    int width = 0, height = 0;
    int hotX = 0, hotY = 0;
    std::vector<std::uint8_t> color;  // top-down BGRA, stride = width * 4
    std::vector<std::uint8_t> mask;   // 1bpp planes (AND and, for mono cursors, XOR)
    int maskStride = 0;
    int maskHeight = 0;
    bool hasColor = false;
};

bool loadCursorImages(HCURSOR cursor, IconImages& out) {
    ICONINFO ii{};
    if (!GetIconInfo(cursor, &ii)) return false;
    bool ok = false;
    HDC dc = GetDC(nullptr);
    do {
        BITMAP bmColor{}, bmMask{};
        if (ii.hbmColor) {
            if (!GetObjectW(ii.hbmColor, sizeof(bmColor), &bmColor)) break;
            out.width = bmColor.bmWidth;
            out.height = bmColor.bmHeight;
            out.hasColor = true;
        } else if (ii.hbmMask) {
            if (!GetObjectW(ii.hbmMask, sizeof(bmMask), &bmMask)) break;
            out.width = bmMask.bmWidth;
            out.height = bmMask.bmHeight / 2;  // AND plane + XOR plane
        } else {
            break;
        }
        if (out.width <= 0 || out.height <= 0) break;
        out.hotX = static_cast<int>(ii.xHotspot);
        out.hotY = static_cast<int>(ii.yHotspot);

        if (out.hasColor && ii.hbmColor) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = out.width;
            bi.bmiHeader.biHeight = -out.height;  // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            out.color.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
            if (GetDIBits(dc, ii.hbmColor, 0, out.height, out.color.data(), &bi,
                          DIB_RGB_COLORS) != out.height)
                break;
        }
        if (ii.hbmMask) {
            GetObjectW(ii.hbmMask, sizeof(bmMask), &bmMask);  // fill outer decl
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = out.width;
            bi.bmiHeader.biHeight = -bmMask.bmHeight;  // all planes, top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 1;
            bi.bmiHeader.biCompression = BI_RGB;
            out.maskStride = ((out.width + 31) / 32) * 4;
            out.mask.assign(static_cast<size_t>(out.maskStride) * bmMask.bmHeight, 0);
            if (GetDIBits(dc, ii.hbmMask, 0, bmMask.bmHeight, out.mask.data(), &bi,
                          DIB_RGB_COLORS) != bmMask.bmHeight)
                break;
            out.maskHeight = bmMask.bmHeight;
        }
        ok = true;
    } while (false);
    ReleaseDC(nullptr, dc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok;
}

}  // namespace

void overlayCursor(std::uint8_t* px, int w, int h, int frameOriginX, int frameOriginY) {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return;
    IconImages ic;
    if (!loadCursorImages(ci.hCursor, ic)) return;

    auto maskBit = [&ic](int x, int y) -> int {
        if (y < 0 || y >= ic.maskHeight) return 1;
        return (ic.mask[static_cast<size_t>(y) * ic.maskStride + (x >> 3)] >>
                (7 - (x & 7))) &
               1;
    };

    const int x0 = ci.ptScreenPos.x - ic.hotX - frameOriginX;
    const int y0 = ci.ptScreenPos.y - ic.hotY - frameOriginY;
    const int yStart = y0 < 0 ? -y0 : 0;
    const int xStart = x0 < 0 ? -x0 : 0;

    for (int iy = yStart; iy < ic.height && y0 + iy < h; ++iy) {
        for (int ix = xStart; ix < ic.width && x0 + ix < w; ++ix) {
            std::uint8_t* d = px + (static_cast<size_t>(y0 + iy) * w + (x0 + ix)) * 4;
            if (ic.hasColor) {
                const std::uint8_t* s =
                    ic.color.data() + (static_cast<size_t>(iy) * ic.width + ix) * 4;
                const unsigned a = s[3];
                if (a != 0) {  // alpha-blend
                    d[0] = static_cast<std::uint8_t>((s[0] * a + d[0] * (255 - a) + 127) / 255);
                    d[1] = static_cast<std::uint8_t>((s[1] * a + d[1] * (255 - a) + 127) / 255);
                    d[2] = static_cast<std::uint8_t>((s[2] * a + d[2] * (255 - a) + 127) / 255);
                    d[3] = 255;
                } else if (maskBit(ix, iy) == 0) {  // AND mask: 0 = draw opaque color
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    d[3] = 255;
                }
            } else {
                // Monochrome cursor: AND plane + XOR plane.
                const int andBit = maskBit(ix, iy);
                const int xorBit = maskBit(ix, iy + ic.height);
                if (andBit == 0) {
                    const std::uint8_t v = xorBit ? 255 : 0;
                    d[0] = d[1] = d[2] = v;
                    d[3] = 255;
                } else if (xorBit) {
                    d[0] = static_cast<std::uint8_t>(~d[0]);
                    d[1] = static_cast<std::uint8_t>(~d[1]);
                    d[2] = static_cast<std::uint8_t>(~d[2]);
                }
            }
        }
    }
}
