#include "encoder.h"

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <wrl/client.h>

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

std::string hrText(const char* what, HRESULT hr) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

}  // namespace

struct VideoEncoder::Impl {
    ComPtr<IMFSinkWriter> writer;
    int width = 0, height = 0, stride = 0;
    DWORD streamIndex = 0;
    std::int64_t nextPts100ns = 0;
    std::uint64_t frames = 0;
    bool opened = false;
};

VideoEncoder::VideoEncoder() = default;
VideoEncoder::~VideoEncoder() = default;

bool VideoEncoder::open(const std::wstring& filePath, int width, int height, int fps,
                        std::uint64_t bitrateBps, std::string& err) {
    impl_ = std::make_unique<Impl>();
    impl_->width = width;
    impl_->height = height;
    impl_->stride = width * 4;

    HRESULT hr = MFStartup(MF_VERSION, 0);
    if (FAILED(hr)) {
        err = hrText("MFStartup", hr);
        return false;
    }

    ComPtr<IMFAttributes> attrs;
    hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr)) {
        err = hrText("MFCreateAttributes", hr);
        return false;
    }
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    hr = MFCreateSinkWriterFromURL(filePath.c_str(), nullptr, attrs.Get(),
                                   &impl_->writer);
    if (FAILED(hr)) {
        err = hrText("MFCreateSinkWriterFromURL", hr);
        return false;
    }

    // Output: H.264 (the target format is set when adding the stream).
    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, fps, 1);
    outType->SetUINT64(MF_MT_AVG_BITRATE, bitrateBps);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    DWORD streamIndex = 0;
    hr = impl_->writer->AddStream(outType.Get(), &streamIndex);
    if (FAILED(hr)) {
        err = hrText("AddStream(H264)", hr);
        return false;
    }
    impl_->streamIndex = streamIndex;

    // Input: top-down RGB32 (auto-converted to NV12 by the sink writer).
    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, fps, 1);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inType->SetUINT32(MF_MT_DEFAULT_STRIDE, impl_->stride);
    hr = impl_->writer->SetInputMediaType(impl_->streamIndex, inType.Get(), nullptr);
    if (FAILED(hr)) {
        err = hrText("SetInputMediaType(RGB32)", hr);
        return false;
    }

    hr = impl_->writer->BeginWriting();
    if (FAILED(hr)) {
        err = hrText("BeginWriting", hr);
        return false;
    }
    impl_->opened = true;
    return true;
}

bool VideoEncoder::writeFrame(const std::uint8_t* bgra, std::int64_t duration100ns,
                              std::string& err) {
    if (!impl_ || !impl_->opened) {
        err = "encoder not open";
        return false;
    }

    ComPtr<IMFMediaBuffer> buffer;
    const DWORD bufLen = static_cast<DWORD>(impl_->stride) * impl_->height;
    HRESULT hr = MFCreateMemoryBuffer(bufLen, &buffer);
    if (FAILED(hr)) {
        err = hrText("MFCreateMemoryBuffer", hr);
        return false;
    }

    // Rows are tightly packed (stride == width * 4), so a single copy suffices.
    BYTE* dst = nullptr;
    hr = buffer->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr)) {
        err = hrText("Lock", hr);
        return false;
    }
    memcpy(dst, bgra, bufLen);
    buffer->Unlock();
    buffer->SetCurrentLength(bufLen);

    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(impl_->nextPts100ns);
    sample->SetSampleDuration(duration100ns);
    impl_->nextPts100ns += duration100ns;

    hr = impl_->writer->WriteSample(impl_->streamIndex, sample.Get());
    if (FAILED(hr)) {
        err = hrText("WriteSample", hr);
        return false;
    }
    ++impl_->frames;
    return true;
}

bool VideoEncoder::finish(std::string& err) {
    if (!impl_ || !impl_->opened) return true;
    HRESULT hr = impl_->writer->Finalize();
    if (FAILED(hr)) {
        err = hrText("Finalize", hr);
        return false;
    }
    impl_->writer.Reset();
    impl_->opened = false;
    MFShutdown();
    return true;
}

std::uint64_t VideoEncoder::frameCount() const {
    return impl_ ? impl_->frames : 0;
}

std::uint64_t VideoEncoder::bytesWritten() const {
    if (!impl_ || !impl_->opened) return 0;
    MF_SINK_WRITER_STATISTICS st{};
#if defined(__MINGW32__)
    st.cb = sizeof(st);  // MinGW-w64 headers use the older field names
#else
    st.cbSize = sizeof(st);
#endif
    if (FAILED(impl_->writer->GetStatistics(impl_->streamIndex, &st))) return 0;
#if defined(__MINGW32__)
    return st.qwByteCountProcessed;
#else
    return st.qwByteCount;
#endif
}
