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

// --- Win32 path helpers ------------------------------------------------------

bool dirExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::uint64_t fileSizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return 0;
    return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
           info.nFileSizeLow;
}

bool ensureDirPath(const std::wstring& dir) {
    if (dir.empty() || dirExists(dir)) return true;
    std::wstring prefix;
    int seps = 0;
    for (size_t i = 0; i < dir.size(); ++i) {
        const wchar_t c = dir[i];
        if (c == L'\\' || c == L'/') {
            const bool uncLead = !prefix.empty() && prefix[0] == L'\\' && seps < 3;
            // Create the prefix for real components only: skip drive roots
            // ("C:" / "C:\") and UNC leads ("\\server\share").
            if (prefix.size() > 2 && !uncLead) {
                if (!CreateDirectoryW(prefix.c_str(), nullptr) &&
                    GetLastError() != ERROR_ALREADY_EXISTS)
                    return false;
            }
            ++seps;
        }
        prefix += c;
    }
    if (prefix.size() > 2 &&
        !(prefix[0] == L'\\' && seps < 4)) {
        if (!CreateDirectoryW(prefix.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    return true;
}

std::wstring parentPathOf(const std::wstring& path) {
    const size_t n = path.find_last_of(L"\\/");
    if (n == std::wstring::npos || n < 3) return L"";  // no parent / drive root
    return path.substr(0, n);
}

std::wstring fileNameOf(const std::wstring& path) {
    const size_t n = path.find_last_of(L"\\/");
    return n == std::wstring::npos ? path : path.substr(n + 1);
}

std::uint64_t autoBitrate(int width, int height, int fps) {
    auto b = static_cast<std::uint64_t>(
        static_cast<double>(width) * height * fps * 0.12);
    if (b < 2'000'000) b = 2'000'000;
    if (b > 40'000'000) b = 40'000'000;
    return b;
}

namespace {

// Expands date/time tokens left-to-right: YYYY YY MM DD HH SS; MM right
// after HH (separators allowed) means minutes, otherwise month. Composite
// forms like YYYYMMDD and HHMMSS fall out naturally: YYYY+MM+DD, HH+MM+SS.
std::wstring expandDateTimeTokens(std::wstring s, const std::tm& tm) {
    wchar_t year[8], yy[4], mon[4], day[4], hour[4], minute[4], sec[4];
    swprintf(year, 8, L"%04d", tm.tm_year + 1900);
    swprintf(yy, 4, L"%02d", (tm.tm_year + 1900) % 100);
    swprintf(mon, 4, L"%02d", tm.tm_mon + 1);
    swprintf(day, 4, L"%02d", tm.tm_mday);
    swprintf(hour, 4, L"%02d", tm.tm_hour);
    swprintf(minute, 4, L"%02d", tm.tm_min);
    swprintf(sec, 4, L"%02d", tm.tm_sec);

    std::wstring out;
    out.reserve(s.size() + 8);
    bool afterHH = false;  // HH matched, separators allowed since
    for (size_t i = 0; i < s.size();) {
        const wchar_t c = s[i];
        if (c == L'Y' && s.compare(i, 4, L"YYYY") == 0) {
            out += year;
            i += 4;
            afterHH = false;
        } else if (c == L'Y' && s.compare(i, 2, L"YY") == 0) {
            out += yy;
            i += 2;
            afterHH = false;
        } else if (c == L'H' && s.compare(i, 2, L"HH") == 0) {
            out += hour;
            i += 2;
            afterHH = true;
        } else if (c == L'D' && s.compare(i, 2, L"DD") == 0) {
            out += day;
            i += 2;
            afterHH = false;
        } else if (c == L'S' && s.compare(i, 2, L"SS") == 0) {
            out += sec;
            i += 2;
            afterHH = false;
        } else if (c == L'M' && s.compare(i, 2, L"MM") == 0) {
            out += afterHH ? minute : mon;
            i += 2;  // MM keeps the HH context (HH-MM-SS stays consistent)
        } else {
            if (iswalnum(c) == 0) {
                // separator: HH context survives ("HH:MM", "HH-MM-SS")
            } else {
                afterHH = false;  // any literal letter/digit resets the context
            }
            out += c;
            ++i;
        }
    }
    return out;
}

}  // namespace

std::wstring defaultRecordingTemplate() {
    // Token-based and user-portable: CAPTURES expands to the Captures known
    // folder at save time (see expandRecordingPath).
    return L"CAPTURES\\Capture-YYYYMMDD-HHMMSS.mp4";
}

namespace {

// Known-folder GUIDs (FOLDERID_*). Captures/Screenshots are Windows 10
// folders missing from some MinGW-w64 headers, so spelled out here.
constexpr GUID kFolderVideos = {
    0x18989b1d, 0x99b5, 0x455b, {0x84, 0x1c, 0xab, 0x7c, 0x74, 0xe4, 0xdd, 0xfc}};
constexpr GUID kFolderPictures = {
    0x33e28130, 0x4e1e, 0x4676, {0x8d, 0x35, 0xa9, 0x83, 0x95, 0xc3, 0xbc, 0x3b}};
constexpr GUID kFolderCaptures = {
    0xedc0fe71, 0x98d8, 0x4f4a, {0xb9, 0x20, 0xc8, 0xdc, 0x13, 0x3c, 0xb1, 0x65}};
constexpr GUID kFolderScreenshots = {
    0xb7bede81, 0xdf94, 0x4682, {0xa7, 0xd8, 0x57, 0xa5, 0x26, 0x20, 0xb8, 0x6f}};

std::wstring knownFolderPath(const GUID& id) {
    wchar_t* p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
        std::wstring dir = p;
        CoTaskMemFree(p);
        return dir;
    }
    return {};
}

bool iequalsW(const std::wstring& a, const wchar_t* b) {
    return _wcsicmp(a.c_str(), b) == 0;
}

// Expands a leading VIDEOS/CAPTURES/PICTURES/SCREENSHOTS path component to
// its known-folder location. Returns false when the path has no such token.
bool expandRootToken(std::wstring& p) {
    const size_t sep = p.find_first_of(L"\\/");
    const std::wstring root =
        sep == std::wstring::npos ? p : p.substr(0, sep);
    const std::wstring rest =
        sep == std::wstring::npos ? L"" : p.substr(sep + 1);

    std::wstring dir;
    if (iequalsW(root, L"VIDEOS")) {
        dir = knownFolderPath(kFolderVideos);
    } else if (iequalsW(root, L"CAPTURES")) {
        // Fallback when the known folder is not registered: <Videos>\Captures.
        dir = knownFolderPath(kFolderCaptures);
        if (dir.empty()) {
            const std::wstring videos = knownFolderPath(kFolderVideos);
            if (!videos.empty()) dir = videos + L"\\Captures";
        }
    } else if (iequalsW(root, L"PICTURES")) {
        dir = knownFolderPath(kFolderPictures);
    } else if (iequalsW(root, L"SCREENSHOTS")) {
        dir = knownFolderPath(kFolderScreenshots);
        if (dir.empty()) {
            const std::wstring pics = knownFolderPath(kFolderPictures);
            if (!pics.empty()) dir = pics + L"\\Screenshots";
        }
    } else {
        return false;
    }
    if (dir.empty()) return false;

    p = rest.empty() ? dir : dir + L"\\" + rest;
    return true;
}

}  // namespace

std::wstring expandRecordingPath(const std::wstring& path) {
    std::wstring p = path;
    expandRootToken(p);  // leading known-folder token, if any
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    return expandDateTimeTokens(std::move(p), tm);
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
