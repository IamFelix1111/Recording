#include "recorder.h"

#include <windows.h>
#include <objbase.h>
#include <process.h>
#include <shlobj.h>

#include <cwchar>
#include <cwctype>
#include <ctime>
#include <memory>
#include <utility>
#include <vector>

#include "capture.h"
#include "encoder.h"
#include "fsutil.h"

// --- QPC clock (steady, no <chrono> runtime bits) ---------------------------

namespace {
const long long g_qpcFreq = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}();
}  // namespace

static long long qpcNs() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<long long>(c.QuadPart * 1'000'000'000LL / g_qpcFreq);
}


std::uint64_t autoBitrate(int width, int height, int fps) {
    auto b = static_cast<std::uint64_t>(
        static_cast<double>(width) * height * fps * 0.12);
    if (b < 2'000'000) b = 2'000'000;
    if (b > 40'000'000) b = 40'000'000;
    return b;
}

RecorderSession::~RecorderSession() {
    requestStop();
    if (worker_) {
        WaitForSingleObject(worker_, INFINITE);
        CloseHandle(worker_);
        worker_ = nullptr;
    }
}

void RecorderSession::setLastError(const std::string& e) {
    AcquireSRWLockExclusive(&errLock_);
    lastError_ = e;
    ReleaseSRWLockExclusive(&errLock_);
}

std::string RecorderSession::lastError() const {
    AcquireSRWLockShared(&errLock_);
    std::string e = lastError_;
    ReleaseSRWLockShared(&errLock_);
    return e;
}

namespace {
unsigned __stdcall recorderWorkerTrampoline(void* arg) {
    static_cast<RecorderSession*>(arg)->worker();
    return 0;
}
}  // namespace

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
    initErr_.clear();
    drawCursor_.store(s.drawCursor);

    initEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual reset
    if (!initEvent_) {
        err = "CreateEvent failed";
        return false;
    }
    running_ = true;
    worker_ = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, recorderWorkerTrampoline, this, 0, nullptr));
    if (!worker_) {
        CloseHandle(initEvent_);
        initEvent_ = nullptr;
        running_ = false;
        err = "_beginthreadex failed";
        return false;
    }

    WaitForSingleObject(initEvent_, INFINITE);
    CloseHandle(initEvent_);
    initEvent_ = nullptr;
    err = initErr_;  // "" on success
    if (!err.empty()) {
        WaitForSingleObject(worker_, INFINITE);
        CloseHandle(worker_);
        worker_ = nullptr;
        running_ = false;
        return false;
    }
    return true;
}

void RecorderSession::requestPauseToggle() { pauseRequested_ = !pauseRequested_.load(); }

void RecorderSession::requestStop() { stopRequested_ = true; }

void RecorderSession::worker() {
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
        if (const std::wstring parent = parentPathOf(settings_.outputPath);
            !parent.empty())
            ensureDirPath(parent);

        VideoEncoder encoder;
        if (!encoder.open(settings_.outputPath, cap->width(), cap->height(), settings_.fps,
                          bitrate, err))
            break;

        ok = true;
        initErr_.clear();
        SetEvent(initEvent_);
        runLoop(*cap, encoder);

        std::string ferr;
        if (!encoder.finish(ferr)) {
            setLastError(ferr);
            statHadError = true;
        }
        // Prefer the final flushed file size when available.
        const auto bytes = fileSizeOf(settings_.outputPath);
        if (bytes != 0)
            statBytes = bytes;
        else
            statBytes = encoder.bytesWritten();
    } while (false);

    if (!ok) {
        initErr_ = err;
        SetEvent(initEvent_);
    }
    if (comInit) CoUninitialize();
    running_ = false;
    if (onFinished) onFinished();
}

void RecorderSession::runLoop(IScreenCapture& cap, VideoEncoder& encoder) {
    const int fps = settings_.fps;
    const std::int64_t intervalNs = 1'000'000'000LL / fps;  // QPC nanoseconds
    std::int64_t deadline = qpcNs();
    std::int64_t lastWall = deadline - intervalNs;
    std::int64_t activeStart = deadline;
    std::int64_t lastStat = deadline;
    std::int64_t activeNs = 0;
    std::uint64_t frames = 0;
    std::uint64_t lastFrames = 0;
    std::string err;
    bool wasPaused = false;

    auto sleepUntil = [](std::int64_t t) {
        const std::int64_t d = t - qpcNs();
        if (d <= 0) return;
        Sleep(static_cast<DWORD>(d / 1'000'000));
    };

    auto totalActiveNs = [&]() -> std::int64_t {
        return activeNs + (qpcNs() - activeStart);
    };

    while (!stopRequested_.load()) {
        cap.setDrawCursor(drawCursor_.load());  // live option from the UI
        if (pauseRequested_.load()) {
            if (!wasPaused) {
                activeNs += qpcNs() - activeStart;
                paused_ = true;
                wasPaused = true;
                deadline = qpcNs();
            }
            Sleep(50);
            continue;
        }
        if (wasPaused) {
            wasPaused = false;
            paused_ = false;
            const std::int64_t now = qpcNs();
            activeStart = now;
            lastWall = now - intervalNs;
            deadline = now;
            lastStat = now;  // fresh fps window after resume
            lastFrames = frames;
        }

        const std::int64_t now = qpcNs();
        int timeoutMs = 2;
        if (deadline > now) {
            timeoutMs = static_cast<int>((deadline - now) / 1'000'000);
            if (timeoutMs > 100) timeoutMs = 100;
            if (timeoutMs < 1) timeoutMs = 1;
        }

        const VideoFrame* frame = cap.captureFrame(timeoutMs, err);
        if (!frame) {
            setLastError("capture error: " + err);
            statHadError = true;
            break;
        }

        std::int64_t dur = qpcNs() - lastWall;
        if (dur < intervalNs / 2) dur = intervalNs / 2;
        if (dur > intervalNs * 10) dur = intervalNs * 10;
        const std::int64_t dur100ns = dur / 100;

        if (!encoder.writeFrame(frame->data, dur100ns, err)) {
            setLastError("encode error: " + err);
            statHadError = true;
            break;
        }
        lastWall = qpcNs();
        ++frames;
        statFrames = frames;

        deadline += intervalNs;
        if (qpcNs() - deadline > intervalNs) deadline = qpcNs();  // fell behind
        sleepUntil(deadline);

        if (qpcNs() - lastStat >= 1'000'000'000LL) {
            const std::int64_t total = totalActiveNs();
            activeNs += qpcNs() - activeStart;
            activeStart = qpcNs();
            statActiveNs = total;
            // Divide by the real elapsed window (1.00-1.03 s) so the shown
            // fps carries a meaningful fractional part.
            const std::int64_t now2 = qpcNs();
            const double dt = static_cast<double>(now2 - lastStat) / 1e9;
            statFps = dt > 0.25 ? static_cast<double>(frames - lastFrames) / dt
                                : statFps.load();
            // Average from the same snapshot (frames and time are consistent).
            const double totalSecs = static_cast<double>(total) / 1e9;
            statAvgFps =
                totalSecs > 0.5 ? static_cast<double>(frames) / totalSecs : 0.0;
            lastFrames = frames;
            // Real-time size: encoder byte count; fall back to flushed file size.
            const auto live = encoder.bytesWritten();
            const auto flushed = fileSizeOf(settings_.outputPath);
            statBytes = live > flushed ? live : flushed;
            lastStat = now2;
        }
    }

    const std::int64_t total = totalActiveNs();
    statActiveNs = total;
    const double secs = static_cast<double>(total) / 1e9;
    statFps = secs > 0.5 ? static_cast<double>(frames) / secs : 0.0;
    statAvgFps = statFps.load();
}
