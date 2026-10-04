// Windows screen recorder - command line front-end.
//   Capture: DXGI Desktop Duplication with automatic GDI fallback, cursor overlay.
//   Encode : Media Foundation H.264 -> MP4 (hardware accelerated when available).
//   Control: F9 pause/resume, F10 or Ctrl+C stop.

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "capture.h"
#include "recorder.h"

namespace fs = std::filesystem;

namespace {

std::atomic<DWORD> g_hotkeyThreadId{0};
RecorderSession* g_session = nullptr;  // valid only while recording

BOOL WINAPI consoleCtrlHandler(DWORD) {
    if (g_session) g_session->requestStop();
    return TRUE;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

void hotkeyLoop() {
    g_hotkeyThreadId = GetCurrentThreadId();
    if (!RegisterHotKey(nullptr, 1, 0, VK_F9))
        std::cerr << "warning: could not register F9 (pause) hotkey\n";
    if (!RegisterHotKey(nullptr, 2, 0, VK_F10))
        std::cerr << "warning: could not register F10 (stop) hotkey\n";
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            if (!g_session) continue;
            if (msg.wParam == 1)
                g_session->requestPauseToggle();
            else if (msg.wParam == 2)
                g_session->requestStop();
        }
    }
}

void stopHotkeyThread(std::thread& t) {
    DWORD id = g_hotkeyThreadId.load();
    if (id) PostThreadMessageW(id, WM_QUIT, 0, 0);
    if (t.joinable()) t.join();
}

void printUsage() {
    std::cout
        << "Usage: Recording [options]\n"
        << "  -o, --output <file>      output MP4 file (default: recording_<timestamp>.mp4)\n"
        << "  -f, --fps <n>            frame rate 1..60 (default 30)\n"
        << "  -m, --monitor <n>        monitor index, see --list (default 0 = primary)\n"
        << "  -a, --all-monitors       record the whole virtual desktop (GDI backend)\n"
        << "  -t, --time <seconds>     stop automatically after n seconds\n"
        << "      --bitrate <kbps>     video bitrate in kbps (default auto)\n"
        << "      --no-cursor          do not draw the mouse cursor\n"
        << "      --gdi                force the GDI capture backend\n"
        << "      --list               list monitors and exit\n"
        << "  -h, --help               show this help\n"
        << "Controls while recording: F9 = pause/resume, F10 or Ctrl+C = stop\n";
}

bool parseInt(const char* s, long& out) {
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    out = v;
    return true;
}

struct Options {
    fs::path output;
    int fps = 30;
    int monitor = 0;
    bool allMonitors = false;
    int durationSec = 0;
    bool drawCursor = true;
    bool listOnly = false;
    bool preferGdi = false;
    std::uint64_t bitrateBps = 0;  // 0 = auto
};

bool parseArgs(int argc, char** argv, Options& opt) {
    auto value = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            std::cerr << "error: missing value for " << argv[i] << "\n";
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        long v = 0;
        if (a == "-h" || a == "--help") {
            printUsage();
            std::exit(0);
        } else if (a == "--list") {
            opt.listOnly = true;
        } else if (a == "-o" || a == "--output") {
            const char* s = value(i);
            if (!s) return false;
            opt.output = fs::path(s);
        } else if (a == "-f" || a == "--fps") {
            const char* s = value(i);
            if (!s || !parseInt(s, v) || v < 1 || v > 60) {
                std::cerr << "error: invalid fps\n";
                return false;
            }
            opt.fps = static_cast<int>(v);
        } else if (a == "-m" || a == "--monitor") {
            const char* s = value(i);
            if (!s || !parseInt(s, v) || v < 0) {
                std::cerr << "error: invalid monitor index\n";
                return false;
            }
            opt.monitor = static_cast<int>(v);
        } else if (a == "-a" || a == "--all-monitors") {
            opt.allMonitors = true;
        } else if (a == "-t" || a == "--time") {
            const char* s = value(i);
            if (!s || !parseInt(s, v) || v <= 0) {
                std::cerr << "error: invalid duration\n";
                return false;
            }
            opt.durationSec = static_cast<int>(v);
        } else if (a == "--bitrate") {
            const char* s = value(i);
            if (!s || !parseInt(s, v) || v < 100) {
                std::cerr << "error: invalid bitrate\n";
                return false;
            }
            opt.bitrateBps = static_cast<std::uint64_t>(v) * 1000;
        } else if (a == "--no-cursor") {
            opt.drawCursor = false;
        } else if (a == "--gdi") {
            opt.preferGdi = true;
        } else {
            std::cerr << "error: unknown option '" << a << "' (see --help)\n";
            return false;
        }
    }
    return true;
}

std::string formatDuration(double secs) {
    int total = static_cast<int>(secs);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", total / 3600, (total / 60) % 60,
             total % 60);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;

    if (opt.listOnly) {
        for (const auto& m : listMonitors()) {
            std::cout << m.index << ": " << m.width << "x" << m.height << "  at (" << m.x
                      << "," << m.y << ")" << (m.primary ? "  [primary]" : "") << "  "
                      << m.device << "\n";
        }
        return 0;
    }

    if (opt.output.empty()) opt.output = defaultRecordingPath();
    if (opt.output.has_parent_path() && !opt.output.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(opt.output.parent_path(), ec);
    }
    const fs::path absPath = fs::absolute(opt.output);

    auto monitors = listMonitors();
    if (!opt.allMonitors && monitors.empty()) {
        std::cerr << "error: no monitors found\n";
        return 1;
    }
    if (!opt.allMonitors &&
        (opt.monitor < 0 || opt.monitor >= static_cast<int>(monitors.size()))) {
        std::cerr << "error: monitor index " << opt.monitor << " out of range\n";
        return 1;
    }

    RecorderSettings settings;
    settings.outputPath = absPath.wstring();
    settings.monitorIndex = opt.monitor;
    settings.allMonitors = opt.allMonitors;
    settings.fps = opt.fps;
    settings.bitrateBps = opt.bitrateBps;
    settings.drawCursor = opt.drawCursor;
    settings.preferGdi = opt.preferGdi;

    RecorderSession session;
    std::string err;
    if (!session.start(settings, err)) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }
    g_session = &session;

    if (!SetConsoleCtrlHandler(consoleCtrlHandler, TRUE))
        std::cerr << "warning: could not install Ctrl+C handler\n";
    std::thread hotkeys(hotkeyLoop);

    std::cout << "Recording -> " << wideToUtf8(absPath.wstring()) << "\n"
              << "Backend: " << session.backendName() << " @ " << session.settings().fps
              << " fps | cursor: " << (opt.drawCursor ? "on" : "off")
              << " | bitrate: " << session.statBitrateBps.load() / 1'000'000.0
              << " Mbps"
              << (opt.durationSec
                      ? (" | auto-stop: " + std::to_string(opt.durationSec) + "s")
                      : "")
              << "\n"
              << "F9 pause/resume | F10 or Ctrl+C stop\n";

    const auto limitNs = static_cast<std::int64_t>(opt.durationSec) * 1'000'000'000LL;
    bool wasPaused = false;
    auto lastPrint = std::chrono::steady_clock::now();
    while (session.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (limitNs > 0 && session.statActiveNs.load() >= limitNs) {
            session.requestStop();
            break;
        }
        const bool paused = session.isPaused();
        if (paused != wasPaused) {
            wasPaused = paused;
            std::cout << "\n" << (paused ? "paused" : "resumed") << "\n";
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPrint < std::chrono::seconds(1)) continue;
        lastPrint = now;
        const double secs = static_cast<double>(session.statActiveNs.load()) / 1e9;
        char line[160];
        snprintf(line, sizeof(line),
                 "\r[%s] frames %6llu | %5.1f fps | %6.1f MB ",
                 formatDuration(secs).c_str(),
                 static_cast<unsigned long long>(session.statFrames.load()),
                 session.statFps.load(),
                 session.statBytes.load() / (1024.0 * 1024.0));
        std::cout << line << std::flush;
    }

    session.requestStop();  // idempotent; destructor joins the worker
    while (session.isRunning())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stopHotkeyThread(hotkeys);
    g_session = nullptr;

    const double totalSecs = static_cast<double>(session.statActiveNs.load()) / 1e9;
    if (session.statHadError.load())
        std::cerr << "warning: " << session.lastError() << "\n";
    std::cout << "\nDone. " << session.statFrames.load() << " frames, "
              << formatDuration(totalSecs) << " total, avg " << session.statFps.load()
              << " fps, " << session.statBytes.load() / (1024.0 * 1024.0) << " MB\n"
              << "Saved to: " << wideToUtf8(absPath.wstring()) << "\n";
    return 0;
}
