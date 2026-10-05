#include "recorder.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include "capture.h"
#include "encoder.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::uint64_t autoBitrate(int width, int height, int fps) {
    std::uint64_t b = static_cast<std::uint64_t>(
        static_cast<double>(width) * height * fps * 0.12);
    if (b < 2'000'000) b = 2'000'000;
    if (b > 40'000'000) b = 40'000'000;
    return b;
}

namespace {

std::wstring videosDir() {
    std::wstring dir = L".";
    wchar_t* videos = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos)) &&
        videos) {
        dir = videos;
        CoTaskMemFree(videos);
    }
    return dir;
}

void replaceAllW(std::wstring& s, const std::wstring& tok, const std::wstring& val) {
    if (tok.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(tok, pos)) != std::wstring::npos) {
        s.replace(pos, tok.size(), val);
        pos += val.size();
    }
}

}  // namespace

std::wstring defaultRecordingTemplate() {
    std::wstring dir = videosDir();
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    return dir + L"Recording-YYYYMMDD-HHMMSS.mp4";
}

std::wstring expandRecordingPath(const std::wstring& path) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t date[16], tod[16];
    swprintf(date, 16, L"%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    swprintf(tod, 16, L"%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::wstring p = path;
    replaceAllW(p, L"YYYYMMDD", date);  // month: tm_mon is 0-based
    replaceAllW(p, L"HHMMSS", tod);     // minute: second pair of HHMMSS
    return p;
}

RecorderSession::~RecorderSession() {
    requestStop();
    if (thread_.joinable()) thread_.join();
}

void RecorderSession::setLastError(const std::string& e) {
    std::lock_guard<std::mutex> lock(errMutex_);
    lastError_ = e;
}

std::string RecorderSession::lastError() const {
    std::lock_guard<std::mutex> lock(errMutex_);
    return lastError_;
}

bool RecorderSession::start(const RecorderSettings& s, std::string& err) {
    settings_ = s;
    stopRequested_ = false;
    pauseRequested_ = false;
    paused_ = false;
    statFrames = 0;
    statActiveNs = 0;
    statBytes = 0;
    statFps = 0.0;
    statAvgFps = 0.0;
    statBitrateBps = 0;
    statHadError = false;

    std::promise<std::string> init;
    auto initFuture = init.get_future();
    running_ = true;
    thread_ = std::thread([this, p = std::move(init)]() mutable {
        worker(std::move(p));
    });

    err = initFuture.get();  // "" on success
    if (!err.empty()) {
        if (thread_.joinable()) thread_.join();
        running_ = false;
        return false;
    }
    return true;
}

void RecorderSession::requestPauseToggle() { pauseRequested_ = !pauseRequested_.load(); }

void RecorderSession::requestStop() { stopRequested_ = true; }

void RecorderSession::worker(std::promise<std::string> initResult) {
    std::string err;
    bool ok = false;
    bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    do {
        if (!comInit) {
            err = "CoInitializeEx failed";
            break;
        }

        std::vector<MonitorInfo> monitors = listMonitors();
        std::unique_ptr<IScreenCapture> cap;
        if (!settings_.allMonitors && !settings_.preferGdi &&
            settings_.monitorIndex >= 0 &&
            settings_.monitorIndex < static_cast<int>(monitors.size())) {
            cap = createDxgiCapture(monitors[settings_.monitorIndex].handle,
                                    settings_.drawCursor, err);
        }
        if (!cap) {
            err.clear();
            cap = createGdiCapture(settings_.monitorIndex, settings_.allMonitors,
                                   settings_.drawCursor, err);
        }
        if (!cap) break;
        int rx = 0, ry = 0, rw = 0, rh = 0;
        cap->screenRect(rx, ry, rw, rh);
        capX = rx;
        capY = ry;
        capW = rw;
        capH = rh;

        const std::uint64_t bitrate = settings_.bitrateBps
                                          ? settings_.bitrateBps
                                          : autoBitrate(cap->width(), cap->height(),
                                                        settings_.fps);
        statBitrateBps = bitrate;

        // Auto-create the output directory (GUI paths may point at new folders).
        {
            std::error_code dec;
            const auto parent = fs::path(settings_.outputPath).parent_path();
            if (!parent.empty()) fs::create_directories(parent, dec);
        }

        VideoEncoder encoder;
        if (!encoder.open(settings_.outputPath, cap->width(), cap->height(), settings_.fps,
                          bitrate, err))
            break;

        ok = true;
        initResult.set_value("");
        runLoop(*cap, encoder);

        std::string ferr;
        if (!encoder.finish(ferr)) {
            setLastError(ferr);
            statHadError = true;
        }
        // Prefer the final flushed file size when available.
        std::error_code ec;
        const auto bytes = fs::file_size(settings_.outputPath, ec);
        if (!ec)
            statBytes = static_cast<std::uint64_t>(bytes);
        else
            statBytes = encoder.bytesWritten();
    } while (false);

    if (!ok) initResult.set_value(err);
    if (comInit) CoUninitialize();
    running_ = false;
    if (onFinished) onFinished();
}

void RecorderSession::runLoop(IScreenCapture& cap, VideoEncoder& encoder) {
    const int fps = settings_.fps;
    const auto interval = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0) / fps);
    auto deadline = Clock::now();
    auto lastWall = Clock::now() - interval;
    auto activeStart = Clock::now();
    auto lastStat = Clock::now();
    std::int64_t activeNs = 0;
    std::uint64_t frames = 0;
    std::uint64_t lastFrames = 0;
    std::string err;
    bool wasPaused = false;

    auto totalActiveNs = [&]() -> std::int64_t {
        return activeNs +
               std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                   activeStart)
                   .count();
    };

    while (!stopRequested_.load()) {
        if (pauseRequested_.load()) {
            if (!wasPaused) {
                activeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - activeStart)
                                .count();
                paused_ = true;
                wasPaused = true;
                deadline = Clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (wasPaused) {
            wasPaused = false;
            paused_ = false;
            activeStart = Clock::now();
            lastWall = Clock::now() - interval;
            deadline = Clock::now();
            lastStat = Clock::now();  // fresh fps window after resume
            lastFrames = frames;
        }

        const auto now = Clock::now();
        int timeoutMs = 2;
        if (deadline > now) {
            timeoutMs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                    .count());
            if (timeoutMs > 100) timeoutMs = 100;
            if (timeoutMs < 1) timeoutMs = 1;
        }

        const VideoFrame* frame = cap.captureFrame(timeoutMs, err);
        if (!frame) {
            setLastError("capture error: " + err);
            statHadError = true;
            break;
        }

        auto dur = Clock::now() - lastWall;
        if (dur < interval / 2) dur = interval / 2;
        if (dur > interval * 10) dur = interval * 10;
        const std::int64_t dur100ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count() / 100;

        if (!encoder.writeFrame(frame->data, dur100ns, err)) {
            setLastError("encode error: " + err);
            statHadError = true;
            break;
        }
        lastWall = Clock::now();
        ++frames;
        statFrames = frames;

        deadline += interval;
        if (Clock::now() - deadline > interval) deadline = Clock::now();  // fell behind
        std::this_thread::sleep_until(deadline);

        if (Clock::now() - lastStat >= std::chrono::seconds(1)) {
            const std::int64_t total = totalActiveNs();
            activeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Clock::now() - activeStart)
                            .count();
            activeStart = Clock::now();
            statActiveNs = total;
            // Divide by the real elapsed window (1.00-1.03 s) so the shown
            // fps carries a meaningful fractional part.
            const double dt =
                std::chrono::duration<double>(Clock::now() - lastStat).count();
            statFps = dt > 0.25 ? static_cast<double>(frames - lastFrames) / dt
                                : statFps.load();
            // Average from the same snapshot (frames and time are consistent).
            const double totalSecs = static_cast<double>(total) / 1e9;
            statAvgFps = totalSecs > 0.5 ? frames / totalSecs : 0.0;
            lastFrames = frames;
            // Real-time size: encoder byte count; fall back to flushed file size.
            const auto live = encoder.bytesWritten();
            std::error_code ec;
            const auto onDisk = fs::file_size(settings_.outputPath, ec);
            const std::uint64_t flushed =
                ec ? 0 : static_cast<std::uint64_t>(onDisk);
            statBytes = live > flushed ? live : flushed;
            lastStat = Clock::now();
        }
    }

    const std::int64_t total = totalActiveNs();
    statActiveNs = total;
    const double secs = static_cast<double>(total) / 1e9;
    statFps = secs > 0.5 ? frames / secs : 0.0;
    statAvgFps = statFps.load();
}
