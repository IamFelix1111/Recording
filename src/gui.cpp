// Windows screen recorder - Win32 GUI front-end (no external dependencies).
// Settings on top, live stats in the middle, start/pause/stop at the bottom.

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include <cstdio>
#include <cwchar>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "capture.h"
#include "recorder.h"

namespace {

constexpr int IDC_MONITOR = 1001;
constexpr int IDC_FPS = 1002;
constexpr int IDC_BITRATE = 1003;
constexpr int IDC_PATH = 1004;
constexpr int IDC_BROWSE = 1005;
constexpr int IDC_CURSOR = 1006;
constexpr int IDC_GDI = 1007;
constexpr int IDC_START = 1008;
constexpr int IDC_PAUSE = 1009;
constexpr int IDC_STOP = 1010;
constexpr UINT WM_APP_FINISHED = WM_APP + 1;
constexpr UINT TIMER_POLL = 1;

struct App {
    HWND wnd = nullptr;
    HWND lblMonitor = nullptr, comboMonitor = nullptr;
    HWND lblFps = nullptr, comboFps = nullptr;
    HWND lblBitrate = nullptr, comboBitrate = nullptr;
    HWND lblPath = nullptr, editPath = nullptr, btnBrowse = nullptr;
    HWND chkCursor = nullptr, chkGdi = nullptr;
    HWND btnStart = nullptr, btnPause = nullptr, btnStop = nullptr;
    HWND stDot = nullptr, stStats = nullptr, stStatus = nullptr;
    HBRUSH brushRec = nullptr, brushPause = nullptr, brushIdle = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    bool quitting = false;
    std::vector<MonitorInfo> monitors;
    std::unique_ptr<RecorderSession> sess;
    bool borderPaused = false;  // color currently shown by the border frame
};
App g;
HWND g_borderWnd = nullptr;

int S(int v) { return MulDiv(v, static_cast<int>(g.dpi), 96); }

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring windowText(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::wstring w(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(h, w.data(), len + 1);
    w.resize(len);
    return w;
}

HWND createCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
               int w, int h, int id) {
    return CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w),
                         S(h), g.wnd,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandleW(nullptr), nullptr);
}

void setUiRecording() {
    for (HWND h : {g.comboMonitor, g.comboFps, g.comboBitrate, g.editPath, g.btnBrowse,
                   g.chkCursor, g.chkGdi, g.btnStart})
        EnableWindow(h, FALSE);
    EnableWindow(g.btnPause, TRUE);
    EnableWindow(g.btnStop, TRUE);
    SetWindowTextW(g.btnPause, L"暂停");
    SetWindowTextW(g.stStatus, utf8ToWide("录制中: " + g.sess->backendName()).c_str());
    InvalidateRect(g.stDot, nullptr, TRUE);
}

void setUiIdle(const std::wstring& status) {
    for (HWND h : {g.comboMonitor, g.comboFps, g.comboBitrate, g.editPath, g.btnBrowse,
                   g.chkCursor, g.chkGdi, g.btnStart})
        EnableWindow(h, TRUE);
    EnableWindow(g.btnPause, FALSE);
    EnableWindow(g.btnStop, FALSE);
    SetWindowTextW(g.btnPause, L"暂停");
    SetWindowTextW(g.stStatus, status.c_str());
    InvalidateRect(g.stDot, nullptr, TRUE);
}

std::wstring formatDuration(double secs) {
    int total = static_cast<int>(secs);
    wchar_t buf[40];
    swprintf(buf, 40, L"%02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    return buf;
}

void showError(const std::string& e) {
    MessageBoxW(g.wnd, utf8ToWide(e).c_str(), L"错误", MB_ICONERROR | MB_OK);
}

// --- red border frame around the recorded region ---------------------------
// Topmost, click-through, and excluded from screen capture
// (WDA_EXCLUDEFROMCAPTURE) so it never appears in the video itself.

LRESULT CALLBACK borderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            const int bw = S(5);
            const bool paused = g.sess && g.sess->isPaused();
            const HBRUSH br =
                CreateSolidBrush(paused ? RGB(230, 170, 0) : RGB(220, 30, 30));
            RECT edges[4] = {
                {0, 0, rc.right, bw},
                {0, rc.bottom - bw, rc.right, rc.bottom},
                {0, 0, bw, rc.bottom},
                {rc.right - bw, 0, rc.right, rc.bottom},
            };
            for (const RECT& r : edges) FillRect(dc, &r, br);
            DeleteObject(br);
            EndPaint(h, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(h, msg, wp, lp);
    }
}

void destroyBorderWindow() {
    if (g_borderWnd) {
        DestroyWindow(g_borderWnd);
        g_borderWnd = nullptr;
    }
}

void createBorderWindow() {
    destroyBorderWindow();
    if (!g.sess) return;
    const int x = g.sess->capX.load(), y = g.sess->capY.load();
    const int w = g.sess->capW.load(), h = g.sess->capH.load();
    if (w <= 0 || h <= 0) return;
    int bw = S(5);
    if (bw * 6 > w || bw * 6 > h) bw = (std::min)(w, h) / 6;  // tiny regions

    g_borderWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE |
            WS_EX_TOOLWINDOW,
        L"RecordingBorder", nullptr, WS_POPUP, x, y, w, h, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!g_borderWnd) return;

    HRGN outer = CreateRectRgn(0, 0, w, h);
    HRGN inner = CreateRectRgn(bw, bw, w - bw, h - bw);
    CombineRgn(outer, outer, inner, RGN_DIFF);
    SetWindowRgn(g_borderWnd, outer, FALSE);  // system owns outer now
    DeleteObject(inner);

    SetLayeredWindowAttributes(g_borderWnd, 0, 255, LWA_ALPHA);
    // Keep the frame out of the captured video (Windows 10 2004+).
    SetWindowDisplayAffinity(g_borderWnd, WDA_EXCLUDEFROMCAPTURE);
    g.borderPaused = g.sess->isPaused();
    ShowWindow(g_borderWnd, SW_SHOWNOACTIVATE);
}

void updateBorderWindow() {
    if (!g_borderWnd || !g.sess) return;
    // Reassert topmost in case another topmost window covered the frame.
    SetWindowPos(g_borderWnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
    const bool paused = g.sess->isPaused();
    if (paused != g.borderPaused) {
        g.borderPaused = paused;
        InvalidateRect(g_borderWnd, nullptr, FALSE);
    }
}

void handleFinished() {
    if (!g.sess) return;
    const bool hadError = g.sess->statHadError.load();
    const std::string err = g.sess->lastError();
    const std::wstring path = g.sess->settings().outputPath;
    const std::uint64_t frames = g.sess->statFrames.load();
    const double secs = static_cast<double>(g.sess->statActiveNs.load()) / 1e9;
    // Prefer the real final file size (MiB, matching Explorer's display).
    std::uint64_t bytes = g.sess->statBytes.load();
    {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(path, ec);
        if (!ec) bytes = static_cast<std::uint64_t>(sz);
    }
    const double mb = bytes / (1024.0 * 1024.0);
    wchar_t stats[128];
    swprintf(stats, 128, L"时长 %ls | 帧数 %llu | 平均 %.1f fps | %.1f MB",
             formatDuration(secs).c_str(),
             static_cast<unsigned long long>(frames),
             g.sess->statFps.load(), mb);
    SetWindowTextW(g.stStats, stats);
    g.sess.reset();  // joins the worker thread
    destroyBorderWindow();

    if (hadError)
        showError(err);
    setUiIdle(hadError ? L"录制出错，文件可能不完整" : (L"已保存: " + path));

    if (g.quitting) DestroyWindow(g.wnd);
}

RecorderSettings settingsFromUi() {
    RecorderSettings s;
    const int monSel = ComboBox_GetCurSel(g.comboMonitor);
    s.allMonitors = (monSel <= 0);
    s.monitorIndex = monSel > 0 ? monSel - 1 : 0;
    wchar_t buf[32];
    ComboBox_GetLBText(g.comboFps, ComboBox_GetCurSel(g.comboFps), buf);
    s.fps = _wtoi(buf);
    if (s.fps < 1 || s.fps > 60) s.fps = 30;
    s.bitrateBps = static_cast<std::uint64_t>(
        ComboBox_GetItemData(g.comboBitrate, ComboBox_GetCurSel(g.comboBitrate)));
    s.outputPath = windowText(g.editPath);
    if (s.outputPath.empty()) s.outputPath = defaultRecordingPath();
    if (s.outputPath.find_last_of(L".") == std::wstring::npos ||
        s.outputPath.find_last_of(L".") < s.outputPath.find_last_of(L"\\")) {
        s.outputPath += L".mp4";  // no extension: add one
    }
    s.drawCursor = (Button_GetCheck(g.chkCursor) == BST_CHECKED);
    s.preferGdi = (Button_GetCheck(g.chkGdi) == BST_CHECKED);
    return s;
}

void saveUiSettings();  // defined below onBrowse
void loadUiSettings();

void onStart() {
    if (g.sess) return;  // already recording
    RecorderSettings s = settingsFromUi();
    saveUiSettings();  // remember the settings used
    auto sess = std::make_unique<RecorderSession>();
    sess->onFinished = [] { PostMessageW(g.wnd, WM_APP_FINISHED, 0, 0); };
    std::string err;
    if (!sess->start(s, err)) {
        showError(err);
        return;
    }
    g.sess = std::move(sess);
    setUiRecording();
    createBorderWindow();
}

void onPoll() {
    if (!g.sess) return;
    if (!g.sess->isRunning()) {
        handleFinished();
        return;
    }
    updateBorderWindow();
    const double secs = static_cast<double>(g.sess->statActiveNs.load()) / 1e9;
    wchar_t stats[128];
    swprintf(stats, 128, L"时长 %ls | 帧数 %llu | %5.1f fps | %.1f MB",
             formatDuration(secs).c_str(),
             static_cast<unsigned long long>(g.sess->statFrames.load()),
             g.sess->statFps.load(), g.sess->statBytes.load() / (1024.0 * 1024.0));
    SetWindowTextW(g.stStats, stats);
    SetWindowTextW(g.btnPause, g.sess->isPaused() ? L"继续" : L"暂停");
    InvalidateRect(g.stDot, nullptr, TRUE);
}

void onBrowse() {
    wchar_t file[MAX_PATH] = L"";
    wchar_t dir[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = L"MP4 视频 (*.mp4)\0*.mp4\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir;
    ofn.lpstrDefExt = L"mp4";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(g.editPath, file);
        saveUiSettings();  // remember the chosen location immediately
    }
}

// --- persisted settings (%APPDATA%\Recording\settings.ini) -----------------

std::wstring configIniPath() {
    std::wstring ini = L"settings.ini";
    wchar_t* base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base)) &&
        base) {
        std::wstring dir = std::wstring(base) + L"\\Recording";
        CreateDirectoryW(dir.c_str(), nullptr);
        ini = dir + L"\\settings.ini";
        CoTaskMemFree(base);
    }
    return ini;
}

void saveUiSettings() {
    const std::wstring ini = configIniPath();
    const std::wstring path = windowText(g.editPath);
    const std::wstring dir = std::filesystem::path(path).parent_path().wstring();
    if (!dir.empty())
        WritePrivateProfileStringW(L"Settings", L"OutputDir", dir.c_str(), ini.c_str());
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", ComboBox_GetCurSel(g.comboMonitor));
    WritePrivateProfileStringW(L"Settings", L"MonitorSel", buf, ini.c_str());
    swprintf(buf, 16, L"%d", GetWindowTextLengthW(g.comboFps) >= 0
                                    ? ComboBox_GetCurSel(g.comboFps)
                                    : 0);
    WritePrivateProfileStringW(L"Settings", L"FpsSel", buf, ini.c_str());
    swprintf(buf, 16, L"%d", ComboBox_GetCurSel(g.comboBitrate));
    WritePrivateProfileStringW(L"Settings", L"BitrateSel", buf, ini.c_str());
    swprintf(buf, 16, L"%d",
             Button_GetCheck(g.chkCursor) == BST_CHECKED ? 1 : 0);
    WritePrivateProfileStringW(L"Settings", L"DrawCursor", buf, ini.c_str());
    swprintf(buf, 16, L"%d", Button_GetCheck(g.chkGdi) == BST_CHECKED ? 1 : 0);
    WritePrivateProfileStringW(L"Settings", L"PreferGdi", buf, ini.c_str());
}

void loadUiSettings() {
    const std::wstring ini = configIniPath();
    wchar_t dir[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"Settings", L"OutputDir", L"", dir, MAX_PATH,
                             ini.c_str());
    if (dir[0] && std::filesystem::is_directory(dir)) {
        // Reuse the remembered folder with a fresh timestamped file name.
        std::filesystem::path p = defaultRecordingPath();
        std::wstring path = std::wstring(dir) + L"\\" + p.filename().wstring();
        SetWindowTextW(g.editPath, path.c_str());
    }
    const int monSel = GetPrivateProfileIntW(L"Settings", L"MonitorSel", -1, ini.c_str());
    if (monSel >= 0 && monSel < ComboBox_GetCount(g.comboMonitor))
        ComboBox_SetCurSel(g.comboMonitor, monSel);
    const int fpsSel = GetPrivateProfileIntW(L"Settings", L"FpsSel", -1, ini.c_str());
    if (fpsSel >= 0 && fpsSel < ComboBox_GetCount(g.comboFps))
        ComboBox_SetCurSel(g.comboFps, fpsSel);
    const int brSel = GetPrivateProfileIntW(L"Settings", L"BitrateSel", -1, ini.c_str());
    if (brSel >= 0 && brSel < ComboBox_GetCount(g.comboBitrate))
        ComboBox_SetCurSel(g.comboBitrate, brSel);
    Button_SetCheck(g.chkCursor,
                    GetPrivateProfileIntW(L"Settings", L"DrawCursor", 1, ini.c_str())
                            ? BST_CHECKED
                            : BST_UNCHECKED);
    Button_SetCheck(g.chkGdi,
                    GetPrivateProfileIntW(L"Settings", L"PreferGdi", 0, ini.c_str())
                            ? BST_CHECKED
                            : BST_UNCHECKED);
}

void fillCombos() {
    g.monitors = listMonitors();
    ComboBox_AddString(g.comboMonitor, L"所有显示器（虚拟桌面）");
    for (const auto& m : g.monitors) {
        wchar_t buf[160];
        swprintf(buf, 160, L"%d: %ldx%ld%ls", m.index, m.width, m.height,
                 m.primary ? L"（主）" : L"");
        ComboBox_AddString(g.comboMonitor, buf);
    }
    ComboBox_SetCurSel(g.comboMonitor, g.monitors.empty() ? 0 : 1);

    for (int f : {10, 15, 24, 30, 60}) {
        wchar_t buf[16];
        swprintf(buf, 16, L"%d", f);
        ComboBox_AddString(g.comboFps, buf);
        if (f == 30) ComboBox_SetCurSel(g.comboFps, ComboBox_GetCount(g.comboFps) - 1);
    }

    struct BitrateItem { const wchar_t* label; std::uint64_t bps; };
    static const BitrateItem items[] = {
        {L"自动", 0},          {L"4 Mbps", 4'000'000},  {L"8 Mbps", 8'000'000},
        {L"12 Mbps", 12'000'000}, {L"20 Mbps", 20'000'000}, {L"40 Mbps", 40'000'000},
    };
    for (const auto& it : items) {
        int i = ComboBox_AddString(g.comboBitrate, it.label);
        ComboBox_SetItemData(g.comboBitrate, i, static_cast<LPARAM>(it.bps));
    }
    ComboBox_SetCurSel(g.comboBitrate, 0);
}

void createControls() {
    g.lblMonitor = createCtl(L"STATIC", L"显示器", SS_CENTERIMAGE, 16, 18, 56, 22, 0);
    g.comboMonitor = createCtl(L"COMBOBOX", nullptr,
                               CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 76, 18, 236,
                               200, IDC_MONITOR);
    g.lblFps = createCtl(L"STATIC", L"帧率", SS_CENTERIMAGE, 322, 18, 34, 22, 0);
    g.comboFps = createCtl(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_TABSTOP, 360, 18,
                           72, 200, IDC_FPS);

    g.lblBitrate = createCtl(L"STATIC", L"码率", SS_CENTERIMAGE, 16, 52, 56, 22, 0);
    g.comboBitrate = createCtl(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_TABSTOP, 76,
                               52, 120, 200, IDC_BITRATE);
    g.chkGdi = createCtl(L"BUTTON", L"使用 GDI 兼容后端", BS_AUTOCHECKBOX | WS_TABSTOP,
                         210, 52, 220, 22, IDC_GDI);

    g.lblPath = createCtl(L"STATIC", L"输出", SS_CENTERIMAGE, 16, 86, 56, 22, 0);
    g.editPath = createCtl(L"EDIT", nullptr,
                           WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 76, 86, 286, 24,
                           IDC_PATH);
    g.btnBrowse = createCtl(L"BUTTON", L"浏览...", WS_TABSTOP, 370, 85, 62, 26,
                            IDC_BROWSE);

    g.chkCursor = createCtl(L"BUTTON", L"录制鼠标指针", BS_AUTOCHECKBOX | WS_TABSTOP,
                            76, 120, 140, 22, IDC_CURSOR);

    createCtl(L"STATIC", nullptr, SS_ETCHEDHORZ, 16, 152, 416, 2, 0);

    g.stDot = createCtl(L"STATIC", L"●", SS_CENTERIMAGE, 16, 164, 24, 24, 0);
    g.stStats = createCtl(L"STATIC", L"时长 00:00:00 | 帧数 0 | 0.0 fps | 0.0 MB",
                          SS_CENTERIMAGE, 46, 164, 386, 24, 0);
    g.stStatus = createCtl(L"STATIC", L"就绪", SS_CENTERIMAGE, 46, 194, 386, 22, 0);

    g.btnStart = createCtl(L"BUTTON", L"开始录制", WS_TABSTOP, 16, 228, 116, 32,
                           IDC_START);
    g.btnPause = createCtl(L"BUTTON", L"暂停", WS_TABSTOP, 142, 228, 90, 32, IDC_PAUSE);
    g.btnStop = createCtl(L"BUTTON", L"停止", WS_TABSTOP, 242, 228, 90, 32, IDC_STOP);

    Button_SetCheck(g.chkCursor, BST_CHECKED);

    for (HWND h : {g.lblMonitor, g.comboMonitor, g.lblFps, g.comboFps, g.lblBitrate,
                   g.comboBitrate, g.chkGdi, g.lblPath, g.editPath, g.btnBrowse,
                   g.chkCursor, g.stDot, g.stStats, g.stStatus, g.btnStart, g.btnPause,
                   g.btnStop})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g.wnd = h;
            createControls();
            fillCombos();
            SetWindowTextW(g.editPath, defaultRecordingPath().c_str());
            loadUiSettings();
            setUiIdle(L"就绪 — 点击「开始录制」");
            SetTimer(h, TIMER_POLL, 200, nullptr);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (code == BN_CLICKED) {
                if (id == IDC_START) {
                    onStart();
                } else if (id == IDC_BROWSE) {
                    onBrowse();
                } else if (id == IDC_PAUSE && g.sess) {
                    g.sess->requestPauseToggle();
                } else if (id == IDC_STOP && g.sess) {
                    g.sess->requestStop();
                    EnableWindow(g.btnPause, FALSE);
                    EnableWindow(g.btnStop, FALSE);
                    SetWindowTextW(g.stStatus, L"正在停止…");
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == TIMER_POLL) onPoll();
            return 0;
        case WM_APP_FINISHED:
            handleFinished();
            return 0;
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(wp);
            const HWND ctl = reinterpret_cast<HWND>(lp);
            SetBkMode(dc, TRANSPARENT);
            if (ctl == g.stDot) {
                const bool running = g.sess && g.sess->isRunning();
                const bool paused = g.sess && g.sess->isPaused();
                SetTextColor(dc, running ? (paused ? RGB(210, 150, 0) : RGB(200, 30, 30))
                                         : RGB(150, 150, 150));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_CLOSE:
            if (g.sess && g.sess->isRunning()) {
                if (!g.quitting) {
                    if (MessageBoxW(g.wnd, L"正在录制，停止并退出？", L"屏幕录制",
                                    MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                        g.quitting = true;
                        g.sess->requestStop();
                        SetWindowTextW(g.stStatus, L"正在停止…");
                    }
                }
                return 0;  // window is destroyed in handleFinished()
            }
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            destroyBorderWindow();
            g.sess.reset();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(h, msg, wp, lp);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);

    g.dpi = GetDpiForSystem();
    if (g.dpi == 0) g.dpi = 96;
    g.font = CreateFontW(-MulDiv(9, static_cast<int>(g.dpi), 72), 0, 0, 0,
                         FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"RecordingGui";
    RegisterClassW(&wc);

    WNDCLASSW wb{};
    wb.lpfnWndProc = borderProc;
    wb.hInstance = inst;
    wb.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wb.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    wb.lpszClassName = L"RecordingBorder";
    RegisterClassW(&wb);

    RECT rc{0, 0, S(448), S(272)};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                     FALSE);
    const int ww = rc.right - rc.left;
    const int wh = rc.bottom - rc.top;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    const int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    HWND wnd = CreateWindowExW(0, L"RecordingGui", L"屏幕录制",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               sx, sy, ww, wh, nullptr, nullptr, inst, nullptr);
    ShowWindow(wnd, show);
    UpdateWindow(wnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g.font) DeleteObject(g.font);
    return static_cast<int>(msg.wParam);
}
