// Tray icon shown while Preparing/Recording/Paused/Saving.
// Interactions: left click = pause/resume (guarded against double-click),
// double click = show window, middle click = stop, right click = menu.
// Commands are routed back through WM_COMMAND (IDM_TRAY_*) so gui.cpp owns
// the recording flow.

#include <windows.h>
#include <shellapi.h>

#include <cwchar>

#include "gui_internal.h"

namespace {

// File-scope (not function-local) so no __cxa_guard is needed.
NOTIFYICONDATAW g_nid{};

NOTIFYICONDATAW& nid() {
    return g_nid;
}

// Timestamp of the last handled double-click: the shell sends a trailing
// WM_LBUTTONUP after WM_LBUTTONDBLCLK, which must not arm the click timer
// again (it would toggle pause/resume right after showing the window).
ULONGLONG g_lastDblClk = 0;

void postCommand(int id) {
    PostMessageW(g.wnd, WM_COMMAND, MAKEWPARAM(id, 0), 0);
}

void showContextMenu() {
    const HMENU menu = CreatePopupMenu();
    // During Preparing/Saving only 退出 is available; pause/stop are
    // meaningless (no session yet / already stopping) and the window is
    // mid-transition, so even showing it is blocked.
    const bool interactions =
        g.state == UiState::Recording || g.state == UiState::Paused;
    AppendMenuW(menu,
                MF_STRING | (interactions ? 0 : MF_GRAYED), IDM_TRAY_SHOW,
                loadStr(IDS_TRAY_SHOW).c_str());
    const bool paused = g.sess && g.sess->isPaused();
    AppendMenuW(menu, MF_STRING | (interactions ? 0 : MF_GRAYED), IDM_TRAY_PAUSE,
                loadStr(paused ? IDS_BTN_RESUME : IDS_BTN_PAUSE).c_str());
    AppendMenuW(menu, MF_STRING | (interactions ? 0 : MF_GRAYED), IDM_TRAY_STOP,
                loadStr(IDS_BTN_STOP).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, loadStr(IDS_TRAY_QUIT).c_str());

    POINT pt{};
    GetCursorPos(&pt);
    // Foreground activation so the menu closes when clicking elsewhere.
    SetForegroundWindow(g.wnd);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                   pt.x, pt.y, 0, g.wnd, nullptr);
    DestroyMenu(menu);
    if (cmd) postCommand(cmd);
}

}  // namespace

void trayShow() {
    NOTIFYICONDATAW& n = nid();
    if (n.hWnd) return;  // already visible
    ensureStateIcons();
    n = {};
    n.cbSize = sizeof(n);
    n.hWnd = g.wnd;
    n.uID = 1;
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    n.uCallbackMessage = WM_APP_TRAY;
    n.hIcon = g.stateIcons[stateIconBucket(g.state)][1];
    wcscpy_s(n.szTip, loadStr(IDS_APP_TITLE).c_str());
    Shell_NotifyIconW(NIM_ADD, &n);
}

void trayHide() {
    NOTIFYICONDATAW& n = nid();
    if (!n.hWnd) return;
    Shell_NotifyIconW(NIM_DELETE, &n);
    n = {};
}

bool trayIsVisible() {
    return nid().hWnd != nullptr;
}

void traySetState(UiState st) {
    NOTIFYICONDATAW& n = nid();
    if (!n.hWnd) return;
    ensureStateIcons();
    n.uFlags = NIF_ICON | NIF_TIP;
    n.hIcon = g.stateIcons[stateIconBucket(st)][1];
    // Tip: plain title, or "屏幕录制 - 录制中/暂停" for the long-lived states.
    if (st == UiState::Recording || st == UiState::Paused) {
        swprintf(n.szTip, 128, loadStr(IDS_TRAY_TIP_FMT).c_str(),
                 loadStr(st == UiState::Recording ? IDS_STATE_RECORDING
                                                  : IDS_STATE_PAUSED)
                     .c_str());
    } else {
        wcscpy_s(n.szTip, loadStr(IDS_APP_TITLE).c_str());
    }
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void traySetBlinkPhase(bool on) {
    NOTIFYICONDATAW& n = nid();
    if (!n.hWnd || g.state != UiState::Recording) return;
    ensureStateIcons();
    n.uFlags = NIF_ICON;
    n.hIcon = g.stateIcons[on ? 1 : kIconBucketRedDim][1];
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

bool trayHandleMessage(WPARAM wp, LPARAM lp) {
    if (wp != 1) return false;  // our single tray id
    switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_MBUTTONUP:
            // Pause/show/stop only make sense while actually recording or
            // paused; during Preparing/Saving they are disabled outright.
            if (g.state == UiState::Preparing || g.state == UiState::Saving)
                return true;
            break;
        default:
            break;
    }
    switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            // Trailing release right after a double-click: ignore, it must
            // not re-arm the single-click timer (bug: paused + double-click
            // resumed recording).
            if (GetTickCount64() - g_lastDblClk <
                static_cast<ULONGLONG>(GetDoubleClickTime()))
                return true;
            // Wait out the double-click window before toggling pause; a
            // double-click cancels the timer and shows the window instead.
            SetTimer(g.wnd, TIMER_TRAYCLICK, GetDoubleClickTime(), nullptr);
            return true;
        case WM_LBUTTONDBLCLK:
            g_lastDblClk = GetTickCount64();
            KillTimer(g.wnd, TIMER_TRAYCLICK);
            postCommand(IDM_TRAY_SHOW);
            return true;
        case WM_MBUTTONUP:
            postCommand(IDM_TRAY_STOP);
            return true;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            showContextMenu();
            return true;
        default:
            return false;
    }
}

bool trayHandleTimer(UINT id) {
    if (id != TIMER_TRAYCLICK) return false;
    KillTimer(g.wnd, TIMER_TRAYCLICK);
    postCommand(IDM_TRAY_PAUSE);
    return true;
}
