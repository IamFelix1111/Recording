// Windows screen recorder - Win32 GUI front-end (no external dependencies).
// Window lifecycle, control layout, recording flow and the border frame.
// State icons live in icons.cpp, settings persistence in settings.cpp.

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <commctrl.h>
#include <commdlg.h>

#include <cstdio>
#include <cwchar>
#include <algorithm>
#include <filesystem>
#include <memory>

#include "gui_internal.h"

App g;
HWND g_borderWnd = nullptr;

int S(int v) { return MulDiv(v, static_cast<int>(g.dpi), 96); }

// Loads a localized string from the STRINGTABLE in gui.rc.
std::wstring loadStr(int id) {
    wchar_t buf[256];
    int n = LoadStringW(GetModuleHandleW(nullptr), id, buf, 256);
    return {buf, static_cast<size_t>(n)};
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring windowText(HWND h) {
    const int len = GetWindowTextLengthW(h);
    std::wstring w(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(h, w.data(), len + 1);
    return {w.data(), static_cast<size_t>(len)};
}

HWND createCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
               int w, int h, int id) {
    return CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w),
                         S(h), g.wnd,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandleW(nullptr), nullptr);
}

void setStatus(UiState st, const std::wstring& text) {
    g.state = st;
    if (g.lastStatus != text) {
        g.lastStatus = text;
        SetWindowTextW(g.stStatus, text.c_str());
    }
    InvalidateRect(g.stDot, nullptr, TRUE);
    updateStateIcons();  // title bar / taskbar icon follows the state color
}

// One vocabulary for the status text, status dot and title-bar prefix.
static std::wstring stateLabel(UiState st) {
    switch (st) {
        case UiState::Ready: return loadStr(IDS_STATE_READY);
        case UiState::Preparing: return loadStr(IDS_STATE_PREPARING);
        case UiState::Recording: return loadStr(IDS_STATE_RECORDING);
        case UiState::Paused: return loadStr(IDS_STATE_PAUSED);
        case UiState::Saving: return loadStr(IDS_STATE_SAVING);
    }
    return loadStr(IDS_STATE_READY);
}

void setStatus(UiState st) { setStatus(st, stateLabel(st)); }

// Cheap text setter: skips the WM_SETTEXT round-trip when unchanged
// (onPoll runs every 200 ms; labels usually keep the same text).
static void setTextIfChanged(HWND ctl, std::wstring& last, const wchar_t* text) {
    if (last == text) return;
    last = text;
    SetWindowTextW(ctl, text);
}

void setUiRecording() {
    for (HWND h : {g.comboMonitor, g.comboFps, g.comboBitrate, g.editPath, g.btnBrowse,
                   g.chkCursor, g.chkGdi})
        EnableWindow(h, FALSE);
    EnableWindow(g.btnStart, TRUE);  // becomes 停止
    EnableWindow(g.btnPause, TRUE);
    SetWindowTextW(g.btnStart, loadStr(IDS_BTN_STOP).c_str());
    SetWindowTextW(g.btnPause, loadStr(IDS_BTN_PAUSE).c_str());
}

void setUiIdle() {
    for (HWND h : {g.comboMonitor, g.comboFps, g.comboBitrate, g.editPath, g.btnBrowse,
                   g.chkCursor, g.chkGdi, g.btnStart})
        EnableWindow(h, TRUE);
    EnableWindow(g.btnPause, FALSE);
    SetWindowTextW(g.btnStart, loadStr(IDS_BTN_START).c_str());
    SetWindowTextW(g.btnPause, loadStr(IDS_BTN_PAUSE).c_str());
    setTextIfChanged(g.wnd, g.lastTitle, loadStr(IDS_APP_TITLE).c_str());
}

std::wstring formatDuration(double secs) {
    int total = static_cast<int>(secs);
    wchar_t buf[40];
    swprintf(buf, 40, L"%02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    return buf;
}

// Title reflects the same states: "屏幕录制" (ready), "(准备中/保存中) 屏幕录制",
// "(录制中/暂停) 屏幕录制 00:00:21 31.0 fps" - duration and fps are live values.
void updateTitle() {
    const std::wstring app = loadStr(IDS_APP_TITLE);
    wchar_t buf[160];
    if ((g.state == UiState::Recording || g.state == UiState::Paused) && g.sess) {
        // Paused: same average fps as the info bar, so the numbers never differ.
        const double fps = g.state == UiState::Paused ? g.sess->statAvgFps.load()
                                                      : g.sess->statFps.load();
        swprintf(buf, 160, loadStr(IDS_TITLE_REC_FMT).c_str(),
                 stateLabel(g.state).c_str(), app.c_str(),
                 formatDuration(static_cast<double>(g.sess->statActiveNs.load()) / 1e9)
                     .c_str(),
                 fps);
        setTextIfChanged(g.wnd, g.lastTitle, buf);
    } else if (g.state == UiState::Preparing || g.state == UiState::Saving) {
        // Valid before the session exists (Preparing) and after stop (Saving).
        swprintf(buf, 160, loadStr(IDS_TITLE_STATE_FMT).c_str(),
                 stateLabel(g.state).c_str(), app.c_str());
        setTextIfChanged(g.wnd, g.lastTitle, buf);
    } else {
        setTextIfChanged(g.wnd, g.lastTitle, app.c_str());
    }
}

// Fixed-layout info bar: status | duration | fps | size, one control each.
// Ready/Preparing show nothing; Paused/Saving/finished show 平均 (average) fps.
void clearStatsLabels() {
    for (auto* last : {&g.lastStatus, &g.lastTime, &g.lastFps, &g.lastSize})
        last->clear();
    SetWindowTextW(g.stStatus, L"");
    SetWindowTextW(g.stTime, L"");
    SetWindowTextW(g.stFps, L"");
    SetWindowTextW(g.stSize, L"");
}

void updateStatsLabels(double recentFps, double avgFps, std::uint64_t bytes,
                       double secs, bool showAvg) {
    wchar_t buf[64];
    swprintf(buf, 64, loadStr(IDS_STATS_TIME_FMT).c_str(),
             formatDuration(secs).c_str());
    setTextIfChanged(g.stTime, g.lastTime, buf);
    swprintf(buf, 64, loadStr(showAvg ? IDS_STATS_AVG_FMT : IDS_STATS_FPS_FMT).c_str(),
             showAvg ? avgFps : recentFps);
    setTextIfChanged(g.stFps, g.lastFps, buf);
    swprintf(buf, 64, loadStr(IDS_STATS_SIZE_FMT).c_str(),
             static_cast<double>(bytes) / (1024.0 * 1024.0));
    setTextIfChanged(g.stSize, g.lastSize, buf);
}

void showError(const std::string& e) {
    MessageBoxW(g.wnd, utf8ToWide(e).c_str(), loadStr(IDS_ERR_TITLE).c_str(),
                MB_ICONERROR | MB_OK);
}

// --- red border frame around the recorded region ---------------------------
// Topmost, click-through, and excluded from screen capture
// (WDA_EXCLUDEFROMCAPTURE) so it never appears in the video itself.

namespace {

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
            HBRUSH br = CreateSolidBrush(paused ? RGB(230, 170, 0) : RGB(220, 30, 30));
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
    const double secs = static_cast<double>(g.sess->statActiveNs.load()) / 1e9;
    // Prefer the real final file size (MiB, matching Explorer's display).
    std::uint64_t bytes = g.sess->statBytes.load();
    {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(path, ec);
        if (!ec) bytes = static_cast<std::uint64_t>(sz);
    }
    updateStatsLabels(g.sess->statFps.load(), g.sess->statAvgFps.load(), bytes, secs,
                      true);
    g.sess.reset();  // joins the worker thread
    destroyBorderWindow();
    setUiIdle();

    if (hadError)
        showError(err);
    setStatus(UiState::Ready,
              hadError ? loadStr(IDS_STATUS_ERROR) : loadStr(IDS_STATUS_SAVED));

    if (g.quitting) DestroyWindow(g.wnd);
}

void onStart() {
    if (g.sess || g.state != UiState::Ready) return;  // already recording/preparing
    RecorderSettings s = settingsFromUi();
    saveUiSettings();  // remember the settings used
    clearStatsLabels();  // fresh stats; the status column shows 准备中
    setStatus(UiState::Preparing);
    updateTitle();
    EnableWindow(g.btnStart, FALSE);  // gray out while preparing
    EnableWindow(g.btnPause, FALSE);
    UpdateWindow(g.wnd);  // paint "准备中" + gray dot before the blocking init
    auto sess = std::make_unique<RecorderSession>();
    sess->onFinished = [] { PostMessageW(g.wnd, WM_APP_FINISHED, 0, 0); };
    std::string err;
    if (!sess->start(s, err)) {
        setUiIdle();
        setStatus(UiState::Ready);
        showError(err);
        return;
    }
    g.sess = std::move(sess);
    setUiRecording();
    setStatus(UiState::Recording);
    updateTitle();
    createBorderWindow();
}

void doStop() {
    if (!g.sess || g.state == UiState::Saving) return;
    g.sess->requestStop();
    setStatus(UiState::Saving);
    updateTitle();
    // Switch the fps readout to the average for 保存中/保存成功.
    updateStatsLabels(g.sess->statFps.load(), g.sess->statAvgFps.load(),
                      g.sess->statBytes.load(),
                      static_cast<double>(g.sess->statActiveNs.load()) / 1e9, true);
    EnableWindow(g.btnStart, FALSE);
    EnableWindow(g.btnPause, FALSE);
}

void onPoll() {
    if (!g.sess) return;
    if (!g.sess->isRunning()) {
        handleFinished();
        return;
    }
    if (g.state == UiState::Saving) return;  // stop requested; keep 保存中 shown
    onBlinkTick();  // blink the taskbar dot while recording
    updateBorderWindow();
    const double secs = static_cast<double>(g.sess->statActiveNs.load()) / 1e9;
    updateStatsLabels(g.sess->statFps.load(), g.sess->statAvgFps.load(),
                      g.sess->statBytes.load(), secs, g.sess->isPaused());
    SetWindowTextW(g.btnPause,
                   loadStr(g.sess->isPaused() ? IDS_BTN_RESUME : IDS_BTN_PAUSE).c_str());
    const UiState st = g.sess->isPaused() ? UiState::Paused : UiState::Recording;
    if (st != g.state) setStatus(st);
    updateTitle();
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

void fillCombos() {
    g.monitors = listMonitors();
    ComboBox_AddString(g.comboMonitor, loadStr(IDS_MONITOR_ALL).c_str());
    const std::wstring primary = loadStr(IDS_MONITOR_PRIMARY);
    for (const auto& m : g.monitors) {
        wchar_t buf[160];
        swprintf(buf, 160, loadStr(IDS_MONITOR_FMT).c_str(), m.index, m.width,
                 m.height, m.primary ? primary.c_str() : L"");
        ComboBox_AddString(g.comboMonitor, buf);
    }
    ComboBox_SetCurSel(g.comboMonitor,
                       g.monitors.empty() ? kMonitorSelAllMonitors
                                          : kMonitorSelFirstMonitor);

    for (int i = 0; i < kFpsChoiceCount; ++i) {
        wchar_t buf[16];
        swprintf(buf, 16, L"%d", kFpsChoices[i]);
        ComboBox_AddString(g.comboFps, buf);
    }
    ComboBox_SetCurSel(g.comboFps, kFpsDefaultSel);

    const std::wstring autoLabel = loadStr(IDS_BITRATE_AUTO);
    for (int i = 0; i < kBitrateChoiceCount; ++i) {
        wchar_t buf[24];
        if (kBitrateMbps[i] == 0) {
            swprintf(buf, 24, L"%ls", autoLabel.c_str());
        } else {
            swprintf(buf, 24, L"%d Mbps", kBitrateMbps[i]);
        }
        const int idx = ComboBox_AddString(g.comboBitrate, buf);
        ComboBox_SetItemData(g.comboBitrate, idx, kBitrateMbps[i] * 1'000'000LL);
    }
    ComboBox_SetCurSel(g.comboBitrate, kBitrateAutoSel);
}

void createControls() {
    g.lblMonitor = createCtl(L"STATIC", loadStr(IDS_LBL_MONITOR).c_str(),
                             SS_CENTERIMAGE, 16, 18, 56, 22, 0);
    g.comboMonitor = createCtl(L"COMBOBOX", nullptr,
                               CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 76, 18, 260,
                               200, IDC_MONITOR);
    g.lblFps = createCtl(L"STATIC", loadStr(IDS_LBL_FPS).c_str(), SS_CENTERIMAGE, 346,
                         18, 34, 22, 0);
    g.comboFps = createCtl(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_TABSTOP, 384, 18,
                           88, 200, IDC_FPS);

    g.lblBitrate = createCtl(L"STATIC", loadStr(IDS_LBL_BITRATE).c_str(), SS_CENTERIMAGE,
                             16, 52, 56, 22, 0);
    g.comboBitrate = createCtl(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_TABSTOP, 76,
                               52, 120, 200, IDC_BITRATE);
    g.chkGdi = createCtl(L"BUTTON", loadStr(IDS_CHK_GDI).c_str(),
                         BS_AUTOCHECKBOX | WS_TABSTOP, 210, 52, 220, 22, IDC_GDI);

    g.lblPath = createCtl(L"STATIC", loadStr(IDS_LBL_PATH).c_str(), SS_CENTERIMAGE, 16,
                          86, 56, 22, 0);
    g.editPath = createCtl(L"EDIT", nullptr,
                           WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 76, 86, 330, 24,
                           IDC_PATH);
    g.btnBrowse = createCtl(L"BUTTON", loadStr(IDS_BROWSE).c_str(), WS_TABSTOP, 414, 85,
                            62, 26, IDC_BROWSE);

    g.chkCursor = createCtl(L"BUTTON", loadStr(IDS_CHK_CURSOR).c_str(),
                            BS_AUTOCHECKBOX | WS_TABSTOP, 76, 120, 140, 22, IDC_CURSOR);

    createCtl(L"STATIC", nullptr, SS_ETCHEDHORZ, 16, 152, 470, 2, 0);

    // Info bar: status | duration | fps | size - four equal columns.
    g.stDot = createCtl(L"STATIC", nullptr, SS_OWNERDRAW, 28, 172, 8, 8, 0);
    g.stStatus = createCtl(L"STATIC", L"", SS_CENTERIMAGE | SS_ENDELLIPSIS, 54, 164,
                           108, 24, 0);
    g.stTime = createCtl(L"STATIC", L"", SS_CENTERIMAGE, 162, 164, 108, 24, 0);
    g.stFps = createCtl(L"STATIC", L"", SS_CENTERIMAGE, 270, 164, 108, 24, 0);
    g.stSize = createCtl(L"STATIC", L"", SS_CENTERIMAGE, 378, 164, 108, 24, 0);

    // One dual-purpose default button (Enter): 开始/停止, plus 暂停.
    g.btnStart = createCtl(L"BUTTON", loadStr(IDS_BTN_START).c_str(),
                           BS_DEFPUSHBUTTON | WS_TABSTOP, 16, 196, 90, 32, IDC_START);
    g.btnPause = createCtl(L"BUTTON", loadStr(IDS_BTN_PAUSE).c_str(), WS_TABSTOP, 116,
                           196, 90, 32, IDC_PAUSE);

    Button_SetCheck(g.chkCursor, BST_CHECKED);

    for (HWND h : {g.lblMonitor, g.comboMonitor, g.lblFps, g.comboFps, g.lblBitrate,
                   g.comboBitrate, g.chkGdi, g.lblPath, g.editPath, g.btnBrowse,
                   g.chkCursor, g.stDot, g.stStatus, g.stTime, g.stFps, g.stSize,
                   g.btnStart, g.btnPause})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
}

}  // namespace

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g.wnd = h;
            g.loading = true;  // suppress saves while seeding defaults below
            createControls();
            fillCombos();
            SetWindowTextW(g.editPath, defaultRecordingTemplate().c_str());
            loadUiSettings();  // resets g.loading at its end
            setUiIdle();
            setStatus(UiState::Ready);
            SetTimer(h, TIMER_POLL, 200, nullptr);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            // Persist the full configuration on every change (not only on start).
            if (!g.loading && !g.sess) {
                if ((code == CBN_SELCHANGE &&
                     (id == IDC_MONITOR || id == IDC_FPS || id == IDC_BITRATE)) ||
                    (code == BN_CLICKED && (id == IDC_CURSOR || id == IDC_GDI)) ||
                    (code == EN_CHANGE && id == IDC_PATH)) {
                    saveUiSettings();
                }
            }
            if (code == BN_CLICKED) {
                if (id == IDC_START) {
                    if (g.sess && g.state != UiState::Saving)
                        doStop();  // button reads 停止 while recording
                    else if (!g.sess && g.state == UiState::Ready)
                        onStart();  // button reads 开始 while idle
                } else if (id == IDC_BROWSE) {
                    onBrowse();
                } else if (id == IDC_PAUSE && g.sess) {
                    g.sess->requestPauseToggle();
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
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (!dis || dis->hwndItem != g.stDot) break;
            drawDotAntialiased(dis->hDC, dis->rcItem, stateColor(g.state));
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            const auto dc = reinterpret_cast<HDC>(wp);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_CLOSE:
            if (g.sess && g.sess->isRunning()) {
                if (!g.quitting) {
                    if (MessageBoxW(g.wnd, loadStr(IDS_CONFIRM_QUIT).c_str(),
                                    loadStr(IDS_APP_TITLE).c_str(),
                                    MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                        g.quitting = true;
                        doStop();
                    }
                }
                return 0;  // window is destroyed in handleFinished()
            }
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            destroyBorderWindow();
            g.sess.reset();
            destroyStateIcons();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(h, msg, wp, lp);
    }
    return DefWindowProcW(h, msg, wp, lp);  // paths that break out of the switch
}

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

    // Pre-generate all state icons; the class icon starts as the Ready (green)
    // one so the window never shows a default or off-state icon.
    ensureStateIcons();

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = g.stateIcons[stateIconBucket(UiState::Ready)][0];
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

    RECT rc{0, 0, S(500), S(240)};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                     FALSE);
    const int ww = rc.right - rc.left;
    const int wh = rc.bottom - rc.top;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    const int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    HWND wnd = CreateWindowExW(0, L"RecordingGui", loadStr(IDS_APP_TITLE).c_str(),
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
