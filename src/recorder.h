#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>

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
// can be polled while recording.
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

    bool isRunning() const { return running_.load(); }
    bool isPaused() const { return paused_.load(); }
    const RecorderSettings& settings() const { return settings_; }
    std::string backendName() const;
    std::string lastError() const;

    // Live stats (poll freely from the UI thread).
    std::atomic<std::uint64_t> statFrames{0};
    std::atomic<std::int64_t> statActiveNs{0};   // active (unpaused) time
    std::atomic<std::uint64_t> statBytes{0};
    std::atomic<double> statFps{0};              // recent fps
    std::atomic<std::uint64_t> statBitrateBps{0};
    std::atomic<bool> statHadError{false};

    // Screen-space capture region (valid after start() returns).
    std::atomic<int> capX{0}, capY{0}, capW{0}, capH{0};

private:
    void worker(std::promise<std::string> initResult);
    void runLoop(class IScreenCapture& cap, class VideoEncoder& encoder);
    void setLastError(const std::string& e);

    RecorderSettings settings_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> pauseRequested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<const char*> backend_{nullptr};
    std::thread thread_;
    mutable std::mutex errMutex_;
    std::string lastError_;
};

// Bitrate suggestion: ~0.12 bits per pixel per frame (1080p30 ~ 7.5 Mbps).
std::uint64_t autoBitrate(int width, int height, int fps);

// recording_<timestamp>.mp4 inside the user's Videos folder (fallback: cwd).
std::wstring defaultRecordingPath();
