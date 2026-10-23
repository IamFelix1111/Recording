// GDI BitBlt capture backend: always available, supports the whole virtual
// desktop (all monitors), used as the fallback (or by explicit choice).

#include <windows.h>

#include <atomic>

#include <vector>

#include "capture.h"

namespace {

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

}  // namespace

std::unique_ptr<IScreenCapture> createGdiCapture(int monitorIndex, bool allMonitors,
                                                 bool drawCursor, std::string& err) {
    auto cap = std::make_unique<GdiCapture>();
    if (!cap->init(monitorIndex, allMonitors, drawCursor, err)) return nullptr;
    return std::unique_ptr<IScreenCapture>(cap.release());
}
