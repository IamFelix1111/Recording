// DXGI Desktop Duplication backend: GPU-efficient per-monitor capture,
// auto-rebuilds the duplication on mode changes / access loss.

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <wrl/client.h>

#include <atomic>

#include <cstring>
#include <vector>

#include "capture.h"
#include "capture_internal.h"

using Microsoft::WRL::ComPtr;

namespace {

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

std::unique_ptr<IScreenCapture> createDxgiCapture(void* hmonitor, bool drawCursor,
                                                  std::string& err) {
    auto cap = std::make_unique<DxgiCapture>();
    if (!cap->init(static_cast<HMONITOR>(hmonitor), drawCursor, err)) return nullptr;
    return cap;
}
