#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

struct RecorderSettings {
    std::wstring outputPath;      // required; .mp4
    int monitorIndex = 0;         // ignored when allMonitors
    bool allMonitors = false;
    int fps = 30;                 // 1..60
    std::uint64_t bitrateBps = 0; // 0 = auto
    bool drawCursor = true;
    bool preferGdi = false;
};

// One recording run: owns the worker thread, capture backend and encoder.
// Control methods are safe to call from any thread; stats are atomics that
// can be polled while recording. Uses Win32 threads/SRWLOCK only (no
// libwinpthread dependency).
class RecorderSession {
public:
    RecorderSession() = default;
    ~RecorderSession();
    RecorderSession(const RecorderSession&) = delete;
    RecorderSession& operator=(const RecorderSession&) = delete;

    // Called on the worker thread when the run ends (success or error).
    std::function<void()> onFinished;

    // Starts the worker; blocks until capture+encoder are initialized.
    // Returns false and fills err on failure.
    bool start(const RecorderSettings& s, std::string& err);

    void requestPauseToggle();
    void requestStop();
    // Live options while recording:
    void setDrawCursor(bool on) { drawCursor_.store(on); }

    bool isRunning() const { return running_.load(); }
    bool isPaused() const { return paused_.load(); }
    const RecorderSettings& settings() const { return settings_; }
    std::string lastError() const;

    // Live stats (poll freely from the UI thread).
    std::atomic<std::uint64_t> statFrames{0};
    std::atomic<std::int64_t> statActiveNs{0};   // active (unpaused) time
    std::atomic<std::uint64_t> statBytes{0};
    std::atomic<double> statFps{0};              // recent fps (1 s window)
    std::atomic<double> statAvgFps{0};           // average fps (consistent snapshot)
    std::atomic<std::uint64_t> statBitrateBps{0};
    std::atomic<bool> statHadError{false};

    // Screen-space capture region (valid after start() returns).
    std::atomic<int> capX{0}, capY{0}, capW{0}, capH{0};

private:
    // Internal: runs on the worker thread (invoked by the trampoline in
    // recorder.cpp). Public visibility is required by the free trampoline.
public:
    void worker();

private:
    void runLoop(class IScreenCapture& cap, class VideoEncoder& encoder);
    void setLastError(const std::string& e);

    RecorderSettings settings_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> pauseRequested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> drawCursor_{true};  // live cursor toggle (per frame)
    HANDLE worker_ = nullptr;       // _beginthreadex handle
    HANDLE initEvent_ = nullptr;    // signaled once init result is ready
    std::string initErr_;           // written before initEvent_ is set
    mutable SRWLOCK errLock_ = SRWLOCK_INIT;
    std::string lastError_;
};

// Bitrate suggestion: ~0.12 bits per pixel per frame (1080p30 ~ 7.5 Mbps).
std::uint64_t autoBitrate(int width, int height, int fps);
