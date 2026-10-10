// Internal shared declarations for the GUI front-end, split across
// gui.cpp (window/lifecycle), icons.cpp (state icons) and settings.cpp
// (settings persistence + UI-to-settings mapping).
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "capture.h"
#include "recorder.h"
#include "resource.h"

// --- control ids / private messages ----------------------------------------
constexpr int IDC_MONITOR = 1001;
constexpr int IDC_FPS = 1002;
constexpr int IDC_BITRATE = 1003;
constexpr int IDC_PATH = 1004;
constexpr int IDC_BROWSE = 1005;
constexpr int IDC_CURSOR = 1006;
constexpr int IDC_GDI = 1007;
constexpr int IDC_START = 1008;  // dual purpose: 开始 (idle) / 停止 (recording)
constexpr int IDC_PAUSE = 1009;
constexpr UINT WM_APP_FINISHED = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;  // Shell_NotifyIcon callback
constexpr UINT TIMER_POLL = 1;
constexpr UINT TIMER_SAVING = 2;      // defer 保存中 visuals (transient skip)
constexpr UINT TIMER_TRAYCLICK = 3;   // tray left-click vs double-click
// States faster than this are not shown (Preparing/Saving visuals).
constexpr DWORD kTransientStateMs = 10;
// Tray context-menu command ids (routed through WM_COMMAND).
constexpr int IDM_TRAY_SHOW = 2001;
constexpr int IDM_TRAY_PAUSE = 2002;
constexpr int IDM_TRAY_STOP = 2003;
constexpr int IDM_TRAY_QUIT = 2004;

// --- combo box index conventions (single source of truth) -------------------
// Monitor combo: entry kMonitorSelAllMonitors = "all monitors" (virtual
// desktop); entries from kMonitorSelFirstMonitor on map 1:1 to monitor index
// (entry kMonitorSelFirstMonitor + i == monitor i).
constexpr int kMonitorSelAllMonitors = 0;
constexpr int kMonitorSelFirstMonitor = 1;
// Fps combo: filled from kFpsChoices in order; the default selection is an
// index into that array (30 fps).
constexpr int kFpsChoices[] = {10, 15, 24, 30, 60};
constexpr int kFpsChoiceCount = 5;
constexpr int kFpsDefaultSel = 3;
// Bitrate combo: kBitrateMbps[sel], where 0 means "auto".
constexpr int kBitrateMbps[] = {0, 4, 8, 12, 20, 40};
constexpr int kBitrateChoiceCount = 6;
constexpr int kBitrateAutoSel = 0;

// UI state machine - drives the status dot color, icons and enabled controls.
//   Ready --(开始)--> Preparing --ok--> Recording <--> Paused
//                                            |
//              Saving <--(停止/退出)---------+
//                 \--finished--> Ready
enum class UiState { Ready, Preparing, Recording, Paused, Saving };

// Icon cache buckets: 0 green, 1 red, 2 yellow, 3 gray, 4 dim red (blink off).
constexpr int kIconBucketCount = 5;
constexpr int kIconBucketRedDim = 4;

struct App {
    HWND wnd = nullptr;
    HWND lblMonitor = nullptr, comboMonitor = nullptr;
    HWND lblFps = nullptr, comboFps = nullptr;
    HWND lblBitrate = nullptr, comboBitrate = nullptr;
    HWND lblPath = nullptr, editPath = nullptr, btnBrowse = nullptr;
    HWND chkCursor = nullptr, chkGdi = nullptr;
    HWND btnStart = nullptr, btnPause = nullptr;  // btnStart toggles 开始/停止
    HWND stDot = nullptr, stStatus = nullptr;
    HWND stTime = nullptr, stFps = nullptr, stSize = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    bool quitting = false;      // quit requested (tray menu): destroy after save
    bool loading = false;  // suppress auto-save while restoring settings
    // Pre-generated state icons, one row per bucket (see kIconBucketCount).
    // [0]=big (taskbar/Alt-Tab), [1]=small (title bar).
    HICON stateIcons[kIconBucketCount][2] = {};
    HICON overlayDot = nullptr;    // taskbar overlay badge (recording, red)
    HICON overlayBlank = nullptr;  // fully transparent overlay (blink "off")
    bool blinkOn = true;  // recording blink phase shown on overlay/tray
    UiState state = UiState::Ready;
    // Last text pushed to each label - skips redundant WM_SETTEXT in onPoll.
    std::wstring lastTitle, lastStatus, lastTime, lastFps, lastSize;
    std::vector<MonitorInfo> monitors;
    std::unique_ptr<RecorderSession> sess;
    bool borderPaused = false;  // color currently shown by the border frame
};
extern App g;
extern HWND g_borderWnd;

// --- shared helpers (defined in gui.cpp) -------------------------------------
int S(int v);  // 96-dpi logical pixel -> device pixel
std::wstring loadStr(int id);
std::wstring utf8ToWide(const std::string& s);
std::wstring windowText(HWND h);
HWND createCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
               int w, int h, int id);

// --- state transitions (gui.cpp) ---------------------------------------------
void setStatus(UiState st, const std::wstring& text);
void setStatus(UiState st);  // uses the standard label for the state
void setUiRecording();
void setUiIdle();
void updateTitle();

// --- info bar (gui.cpp) -------------------------------------------------------
std::wstring formatDuration(double secs);
void clearStatsLabels();
void updateStatsLabels(double recentFps, double avgFps, std::uint64_t bytes,
                       double secs, bool showAvg);
void showError(const std::string& e);

// --- state icons (icons.cpp) --------------------------------------------------
// Single source of truth for the state color (status dot + window icon).
COLORREF stateColor(UiState st);
void drawDotAntialiased(HDC dc, const RECT& rc, COLORREF c);
int stateIconBucket(UiState st);
void ensureStateIcons();
void destroyStateIcons();
void updateStateIcons();
// While recording, blinks the taskbar overlay badge (red dot <-> none) and
// mirrors the phase onto the tray icon (red <-> dim red).
void onBlinkTick();
// Entering the recording state: shows the overlay badge immediately.
void blinkRecordingStart();
// Clears the taskbar overlay badge (e.g. when leaving the recording state).
void clearTaskbarOverlay();
// Releases the ITaskbarList3 reference before CoUninitialize.
void releaseTaskbarCom();

// --- tray icon (tray.cpp) -----------------------------------------------------
// Visible while Preparing/Recording/Paused/Saving; hidden when Ready.
void trayShow();
void trayHide();
[[nodiscard]] bool trayIsVisible();
void traySetState(UiState st);
// Applies the blink phase to the tray icon while recording.
void traySetBlinkPhase(bool on);
// Handles WM_APP_TRAY / TIMER_TRAYCLICK in the main wndProc; true = handled.
bool trayHandleMessage(WPARAM wp, LPARAM lp);
bool trayHandleTimer(UINT id);

// --- settings (settings.cpp) --------------------------------------------------
RecorderSettings settingsFromUi();
std::wstring configIniPath();
void saveUiSettings();
void loadUiSettings();
