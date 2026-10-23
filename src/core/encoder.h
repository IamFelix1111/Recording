#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Media Foundation H.264 sink writer producing an MP4 file.
class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool open(const std::wstring& filePath, int width, int height, int fps,
              std::uint64_t bitrateBps, std::string& err);

    // bgra: top-down 32-bit BGRA with stride = width * 4.
    // duration100ns: display duration of this frame (100 ns units).
    bool writeFrame(const std::uint8_t* bgra, std::int64_t duration100ns, std::string& err);

    bool finish(std::string& err);

    [[nodiscard]] std::uint64_t frameCount() const;

    // Bytes handed to the sink writer so far (more real-time than file size).
    [[nodiscard]] std::uint64_t bytesWritten() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
