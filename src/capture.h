#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct CaptureConfig {
    int monitorIndex = 0;      // monitor to record (see listMonitors)
    bool allMonitors = false;  // record entire virtual desktop (GDI backend only)
    bool drawCursor = true;
};

// Top-down 32-bit BGRA frame owned by the capture backend.
struct VideoFrame {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;  // bytes
    int width = 0;
    int height = 0;
    bool changed = false;  // false = duplicate of the previous frame
};

class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;
    [[nodiscard]] virtual const char* name() const = 0;
    [[nodiscard]] virtual int width() const = 0;
    [[nodiscard]] virtual int height() const = 0;
    // Screen coordinates of the captured region (for UI overlays).
    virtual void screenRect(int& x, int& y, int& w, int& h) const = 0;
    // Blocks up to timeoutMs waiting for a frame. Returns nullptr only on error.
    virtual const VideoFrame* captureFrame(int timeoutMs, std::string& err) = 0;
};

struct MonitorInfo {
    int index = 0;
    long x = 0, y = 0, width = 0, height = 0;
    bool primary = false;
    std::string device;  // \\.\DISPLAY1 etc.
    void* handle = nullptr;  // HMONITOR
};

std::vector<MonitorInfo> listMonitors();

// DXGI Desktop Duplication (Windows 8+, single monitor, GPU-efficient).
std::unique_ptr<IScreenCapture> createDxgiCapture(void* hmonitor, bool drawCursor,
                                                  std::string& err);
// GDI BitBlt (always available; supports multi-monitor virtual desktop).
std::unique_ptr<IScreenCapture> createGdiCapture(int monitorIndex, bool allMonitors,
                                                 bool drawCursor, std::string& err);
