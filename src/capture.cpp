#include "capture.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <wrl/client.h>

#include <atomic>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::string wideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string hrText(HRESULT hr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// ---------------------------------------------------------------------------
// Cursor overlay onto a raw BGRA buffer (used by the DXGI backend; the GDI
// backend draws the cursor with DrawIconEx directly into its memory DC).
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// GDI capture backend
// ---------------------------------------------------------------------------

class GdiCapture : public IScreenCapture {
public:
    ~GdiCapture() override {
        if (memDc_) {
            if (oldBmp_) SelectObject(memDc_, oldBmp_);
            DeleteDC(memDc_);
        }
        if (bmp_) DeleteObject(bmp_);
        if (screenDc_) ReleaseDC(nullptr, screenDc_);
    }

    bool init(int monitorIndex, bool allMonitors, bool drawCursor, std::string& err) {
        drawCursor_.store(drawCursor);
        if (allMonitors) {
            rect_.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
            rect_.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            rect_.right = rect_.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            rect_.bottom = rect_.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        } else {
            auto mons = listMonitors();
            if (monitorIndex < 0 || monitorIndex >= static_cast<int>(mons.size())) {
                err = "monitor index out of range";
                return false;
            }
            auto hmon = static_cast<HMONITOR>(mons[monitorIndex].handle);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(hmon, &mi)) {
                err = "GetMonitorInfoW failed";
                return false;
            }
            rect_ = mi.rcMonitor;
        }
        width_ = static_cast<int>(rect_.right - rect_.left);
        height_ = static_cast<int>(rect_.bottom - rect_.top);
        if (width_ <= 0 || height_ <= 0) {
            err = "invalid capture rectangle";
            return false;
        }

        screenDc_ = GetDC(nullptr);
        memDc_ = CreateCompatibleDC(screenDc_);
        bmp_ = CreateCompatibleBitmap(screenDc_, width_, height_);
        if (!screenDc_ || !memDc_ || !bmp_) {
            err = "GDI resource creation failed";
            return false;
        }
        oldBmp_ = static_cast<HBITMAP>(SelectObject(memDc_, bmp_));

        bmi_ = {};
        bmi_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi_.bmiHeader.biWidth = width_;
        bmi_.bmiHeader.biHeight = -height_;  // negative = top-down
        bmi_.bmiHeader.biPlanes = 1;
        bmi_.bmiHeader.biBitCount = 32;
        bmi_.bmiHeader.biCompression = BI_RGB;

        pixels_.assign(static_cast<size_t>(width_) * height_ * 4, 0);
        frame_.data = pixels_.data();
        frame_.size = pixels_.size();
        frame_.width = width_;
        frame_.height = height_;
        return true;
    }

    [[nodiscard]] const char* name() const override { return "GDI BitBlt"; }
    [[nodiscard]] int width() const override { return width_; }
    [[nodiscard]] int height() const override { return height_; }
    void screenRect(int& x, int& y, int& w, int& h) const override {
        x = static_cast<int>(rect_.left);
        y = static_cast<int>(rect_.top);
        w = width_;
        h = height_;
    }
    void setDrawCursor(bool on) override { drawCursor_.store(on); }

    const VideoFrame* captureFrame(int /*timeoutMs*/, std::string& err) override {
        if (!BitBlt(memDc_, 0, 0, width_, height_, screenDc_, rect_.left, rect_.top,
                    SRCCOPY)) {
            err = "BitBlt failed";
            return nullptr;
        }
        if (drawCursor_.load()) {
            CURSORINFO ci{};
            ci.cbSize = sizeof(ci);
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                DrawIconEx(memDc_, ci.ptScreenPos.x - rect_.left,
                           ci.ptScreenPos.y - rect_.top, ci.hCursor, 0, 0, 0, nullptr,
                           DI_NORMAL);
            }
        }
        // GetDIBits requires the bitmap not be selected into a DC.
        SelectObject(memDc_, oldBmp_);
        const int lines = GetDIBits(memDc_, bmp_, 0, height_, pixels_.data(), &bmi_,
                                    DIB_RGB_COLORS);
        SelectObject(memDc_, bmp_);
        if (lines != height_) {
            err = "GetDIBits failed";
            return nullptr;
        }
        frame_.changed = true;
        return &frame_;
    }

private:
    HDC screenDc_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HBITMAP oldBmp_ = nullptr;
    BITMAPINFO bmi_{};
    RECT rect_{};
    int width_ = 0, height_ = 0;
    std::atomic<bool> drawCursor_{true};
    std::vector<std::uint8_t> pixels_;
    VideoFrame frame_;
};

// ---------------------------------------------------------------------------
// DXGI Desktop Duplication backend
// ---------------------------------------------------------------------------

class DxgiCapture : public IScreenCapture {
public:
    ~DxgiCapture() override = default;

    bool init(HMONITOR target, bool drawCursor, std::string& err) {
        drawCursor_.store(drawCursor);

        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            err = "CreateDXGIFactory1 failed: " + hrText(hr);
            return false;
        }

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
            ComPtr<IDXGIOutput> output;
            for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == target) {
                    output.As(&output1_);
                    break;
                }
                output.Reset();
            }
            if (output1_) break;
            adapter.Reset();
        }
        if (!output1_) {
            err = "monitor not found on any DXGI adapter";
            return false;
        }

        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr,
                               0, D3D11_SDK_VERSION, &device_, nullptr, &context_);
        if (FAILED(hr)) {
            err = "D3D11CreateDevice failed: " + hrText(hr);
            return false;
        }

        DXGI_OUTDUPL_DESC dupDesc{};
        if (!duplicate(err)) return false;
        dup_->GetDesc(&dupDesc);
        width_ = static_cast<int>(dupDesc.ModeDesc.Width);
        height_ = static_cast<int>(dupDesc.ModeDesc.Height);

        DXGI_OUTPUT_DESC outDesc{};
        output1_->GetDesc(&outDesc);
        originX_ = outDesc.DesktopCoordinates.left;
        originY_ = outDesc.DesktopCoordinates.top;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = dupDesc.ModeDesc.Width;
        td.Height = dupDesc.ModeDesc.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device_->CreateTexture2D(&td, nullptr, &staging_);
        if (FAILED(hr)) {
            err = "CreateTexture2D failed: " + hrText(hr);
            return false;
        }

        pixels_.assign(static_cast<size_t>(width_) * height_ * 4, 0);
        frame_.data = pixels_.data();
        frame_.size = pixels_.size();
        frame_.width = width_;
        frame_.height = height_;
        return true;
    }

    [[nodiscard]] const char* name() const override { return "DXGI Desktop Duplication"; }
    [[nodiscard]] int width() const override { return width_; }
    [[nodiscard]] int height() const override { return height_; }
    void screenRect(int& x, int& y, int& w, int& h) const override {
        x = originX_;
        y = originY_;
        w = width_;
        h = height_;
    }
    void setDrawCursor(bool on) override { drawCursor_.store(on); }

    const VideoFrame* captureFrame(int timeoutMs, std::string& err) override {
        if (frameHeld_ && dup_) {
            dup_->ReleaseFrame();
            frameHeld_ = false;
        }
        if (!dup_ && !duplicate(err)) return nullptr;

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            frame_.changed = false;  // no screen update; reuse previous frame
            return &frame_;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED) {
            if (!duplicate(err)) {
                err = "desktop duplication lost: " + err;
                return nullptr;
            }
            frame_.changed = false;
            return &frame_;
        }
        if (FAILED(hr)) {
            err = "AcquireNextFrame failed: " + hrText(hr);
            return nullptr;
        }
        frameHeld_ = true;

        {
            ComPtr<ID3D11Texture2D> desktop;
            if (SUCCEEDED(resource.As(&desktop))) {
                context_->CopyResource(staging_.Get(), desktop.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    const UINT srcPitch = mapped.RowPitch;
                    const size_t rowBytes = static_cast<size_t>(width_) * 4;
                    for (int y = 0; y < height_; ++y) {
                        memcpy(pixels_.data() + static_cast<size_t>(y) * rowBytes,
                               static_cast<const std::uint8_t*>(mapped.pData) +
                                   static_cast<size_t>(y) * srcPitch,
                               rowBytes);
                    }
                    context_->Unmap(staging_.Get(), 0);
                }
            }
        }

        dup_->ReleaseFrame();
        frameHeld_ = false;

        if (drawCursor_.load())
            overlayCursor(pixels_.data(), width_, height_, originX_, originY_);
        frame_.changed = true;
        return &frame_;
    }

private:
    bool duplicate(std::string& err) {
        dup_.Reset();
        for (int attempt = 0; attempt < 3; ++attempt) {
            HRESULT hr = output1_->DuplicateOutput(device_.Get(), &dup_);
            if (SUCCEEDED(hr)) return true;
            if (attempt == 2) err = "DuplicateOutput failed: " + hrText(hr);
            Sleep(100);
        }
        return false;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutput1> output1_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> staging_;
    int width_ = 0, height_ = 0;
    int originX_ = 0, originY_ = 0;
    std::atomic<bool> drawCursor_{true};
    bool frameHeld_ = false;
    std::vector<std::uint8_t> pixels_;
    VideoFrame frame_;
};

}  // namespace

std::vector<MonitorInfo> listMonitors() {
    std::vector<MonitorInfo> mons;
    mons.reserve(4);
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hmon, HDC, LPRECT rect, LPARAM lp) -> BOOL {
            auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hmon, &mi)) {
                MonitorInfo m;
                m.index = static_cast<int>(out->size());
                m.x = mi.rcMonitor.left;
                m.y = mi.rcMonitor.top;
                m.width = mi.rcMonitor.right - mi.rcMonitor.left;
                m.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
                m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                m.device = wideToUtf8(mi.szDevice);
                m.handle = hmon;
                out->push_back(m);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&mons));
    if (!mons.empty() && !mons[0].primary) {
        // Put the primary monitor first so index 0 is predictable.
        auto it = std::ranges::find_if(
            mons, [](const MonitorInfo& m) { return m.primary; });
        if (it != mons.end()) std::swap(mons[0], *it);
        for (size_t i = 0; i < mons.size(); ++i) mons[i].index = static_cast<int>(i);
    }
    return mons;
}

std::unique_ptr<IScreenCapture> createDxgiCapture(void* hmonitor, bool drawCursor,
                                                  std::string& err) {
    auto cap = std::make_unique<DxgiCapture>();
    if (!cap->init(static_cast<HMONITOR>(hmonitor), drawCursor, err)) return nullptr;
    return cap;
}

std::unique_ptr<IScreenCapture> createGdiCapture(int monitorIndex, bool allMonitors,
                                                 bool drawCursor, std::string& err) {
    auto cap = std::make_unique<GdiCapture>();
    if (!cap->init(monitorIndex, allMonitors, drawCursor, err)) return nullptr;
    return std::unique_ptr<IScreenCapture>(cap.release());
}
