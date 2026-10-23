// Windows screen recorder - Win32 GUI front-end (no external dependencies).
// Window lifecycle, control layout, recording flow and the border frame.
// State icons live in icons.cpp, settings persistence in settings.cpp.

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <commctrl.h>
#include <commdlg.h>
#include <process.h>

#include <cstdio>
#include <cwchar>
#include <algorithm>
#include <memory>

#include "gui_internal.h"
#include "fsutil.h"
#include "pathtokens.h"

App g;

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
    // Activity states keep the tray up (it was lost from the transient block
    // in an earlier refactor); Ready relies on handleFinished's trayHide.
    if (st != UiState::Ready && !trayIsVisible()) trayShow();
    traySetState(st);    // applies the state icon/tip
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
    for (HWND h : {g.comboMonitor, g.comboFps, g.comboBitrate, g.chkGdi})
        EnableWindow(h, FALSE);
    // Live options stay enabled while recording: the cursor toggle applies
    // per frame; an edited output path moves the finished file on stop.
    for (HWND h : {g.editPath, g.btnBrowse, g.chkCursor})
        EnableWindow(h, TRUE);
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
            if (id == IDM_TRAY_SHOW) {
                ShowWindow(h, SW_SHOW);
                SetForegroundWindow(h);
                return 0;
            }
            if (id == IDM_TRAY_PAUSE && g.sess) {
                g.sess->requestPauseToggle();
                return 0;
            }
            if (id == IDM_TRAY_STOP && g.sess) {
                doStop();
                return 0;
            }
            if (id == IDM_TRAY_QUIT) {
                // The tray only exists during activity states, so quitting
                // always means "stop, save, then exit".
                const int choice = MessageBoxW(
                    h, loadStr(IDS_CONFIRM_QUIT).c_str(),
                    loadStr(IDS_APP_TITLE).c_str(), MB_OKCANCEL | MB_ICONQUESTION);
                if (choice != IDOK) return 0;
                g.quitting = true;
                if (g.sess && g.sess->isRunning()) {
                    doStop();  // handleFinished destroys the window when done
                } else if (g.state == UiState::Preparing) {
                    // Session not established yet; onStart applies the stop
                    // as soon as the (still running) init finishes.
                } else {
                    g.quitting = false;
                    DestroyWindow(h);
                }
                return 0;
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
                } else if (id == IDC_CURSOR) {
                    // Live option: applies from the next captured frame on.
                    if (g.sess) g.sess->setDrawCursor(
                        Button_GetCheck(g.chkCursor) == BST_CHECKED);
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == TIMER_POLL) {
                onPoll();
            } else if (wp == TIMER_SAVING) {
                // Saving outlasted the transient threshold: show it now
                // (disabled buttons, blue dot/icon, tray, title).
                KillTimer(h, TIMER_SAVING);
                if (g.state == UiState::Saving && g.sess) {
                    EnableWindow(g.btnStart, FALSE);
                    EnableWindow(g.btnPause, FALSE);
                    setStatus(UiState::Saving);
                    updateTitle();
                }
            } else {
                trayHandleTimer(static_cast<UINT>(wp));
            }
            return 0;
        case WM_APP_TRAY:
            trayHandleMessage(wp, lp);
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
            // While the tray is up (preparing/recording/paused/saving) closing
            // the window just hides it; from Ready, close really quits.
            if (trayIsVisible())
                ShowWindow(h, SW_HIDE);
            else
                DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            trayHide();
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
    // STA for shell COM used on this thread (taskbar overlay, tray).
    const bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
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
    registerBorderClass(inst);

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
    releaseTaskbarCom();  // drop the STA ref before CoUninitialize
    if (comInit) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
