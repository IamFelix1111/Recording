// Recording flow: session launch (background init + message pump with
// sub-10ms transient suppression), stop, 200 ms poll, and finish handling
// (stats, edited-path move, UI reset).

#include <windows.h>
#include <windowsx.h>
#include <process.h>

#include <cstdio>

#include <memory>

#include "gui_internal.h"
#include "fsutil.h"
#include "pathtokens.h"

void handleFinished() {
    if (!g.sess) return;
    KillTimer(g.wnd, TIMER_SAVING);  // visuals may have been deferred
    const bool hadError = g.sess->statHadError.load();
    const std::string err = g.sess->lastError();
    const std::wstring path = g.sess->settings().outputPath;
    const double secs = static_cast<double>(g.sess->statActiveNs.load()) / 1e9;
    // Prefer the real final file size (MiB, matching Explorer's display).
    std::uint64_t bytes = g.sess->statBytes.load();
    if (const std::uint64_t onDisk = fileSizeOf(path)) bytes = onDisk;
    updateStatsLabels(g.sess->statFps.load(), g.sess->statAvgFps.load(), bytes, secs,
                      true);
    const std::wstring oldPath = g.sess->settings().outputPath;
    g.sess.reset();  // joins the worker thread
    destroyBorderWindow();

    // The output path may have been edited while recording: the encoder is
    // bound to the original URL, so move the finished file to the new target.
    // Compare RAW box text (not expanded): an untouched token template would
    // otherwise re-expand to a fresh HHMMSS and "move" on every recording.
    const std::wstring nowPathText = windowText(g.editPath);
    std::wstring newPath;
    if (_wcsicmp(nowPathText.c_str(), g.startPathText.c_str()) != 0)
        newPath = expandRecordingPath(nowPathText);
    if (!newPath.empty() && _wcsicmp(newPath.c_str(), oldPath.c_str()) != 0) {
        if (const std::wstring parent = parentPathOf(newPath); !parent.empty())
            ensureDirPath(parent);
        if (!MoveFileExW(oldPath.c_str(), newPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING)) {
            wchar_t msg[MAX_PATH + 128];
            swprintf(msg, MAX_PATH + 128, L"MoveFile failed (%lu)\n%ls ->\n%ls",
                     GetLastError(), oldPath.c_str(), newPath.c_str());
            MessageBoxW(g.wnd, msg, loadStr(IDS_ERR_TITLE).c_str(),
                        MB_ICONWARNING | MB_OK);
        } else if (const std::uint64_t moved = fileSizeOf(newPath)) {
            bytes = moved;
        }
    }

    setUiIdle();

    if (hadError)
        showError(err);
    setStatus(UiState::Ready,
              hadError ? loadStr(IDS_STATUS_ERROR) : loadStr(IDS_STATUS_SAVED));
    trayHide();

    if (g.quitting) {
        g.quitting = false;
        DestroyWindow(g.wnd);
        return;
    }
    // Recording stopped: bring the window back up front.
    ShowWindow(g.wnd, SW_SHOW);
    SetForegroundWindow(g.wnd);
}

namespace {

// Context for the background session-init thread (see onStart).
struct StarterCtx {
    RecorderSession* sess;
    RecorderSettings s;
    bool ok;
    std::string err;
    HANDLE done;
};

unsigned __stdcall starterTrampoline(void* p) {
    auto* c = static_cast<StarterCtx*>(p);
    c->ok = c->sess->start(c->s, c->err);
    SetEvent(c->done);
    return 0;
}

}  // namespace

void onStart() {
    if (g.sess || g.state != UiState::Ready) return;  // already recording/preparing
    RecorderSettings s = settingsFromUi();
    saveUiSettings();  // remember the settings used
    g.startPathText = windowText(g.editPath);  // move-detection baseline
    clearStatsLabels();
    g.state = UiState::Preparing;  // logical state; visuals deferred by 10 ms

    // Run the blocking session init on a helper thread and keep pumping
    // messages: the UI stays responsive, and if init finishes faster than
    // kTransientStateMs no Preparing visuals (disabled buttons, blue dot,
    // icon, tray, title) are shown at all.
    auto sess = std::make_unique<RecorderSession>();
    sess->onFinished = [] { PostMessageW(g.wnd, WM_APP_FINISHED, 0, 0); };
    std::string err;
    bool ok = false;
    bool visualsShown = false;
    bool quitPosted = false;
    const ULONGLONG t0 = GetTickCount64();
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto ctx = std::make_unique<StarterCtx>();
    ctx->sess = sess.get();
    ctx->s = s;
    ctx->done = done;
    StarterCtx* rawCtx = ctx.get();
    HANDLE starter = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, starterTrampoline, rawCtx, 0, nullptr));
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const DWORD wait =
            visualsShown || quitPosted
                ? INFINITE
                : (now - t0 >= kTransientStateMs
                       ? 0
                       : static_cast<DWORD>(kTransientStateMs - (now - t0)));
        const DWORD wr = MsgWaitForMultipleObjects(1, &done, FALSE, wait, QS_ALLINPUT);
        if (wr == WAIT_OBJECT_0) break;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                quitPosted = true;
                continue;  // re-posted after the session settles below
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!visualsShown && !quitPosted && GetTickCount64() - t0 >= kTransientStateMs) {
            visualsShown = true;
            // Disable the session-fixed options (already snapshotted); the
            // live ones (cursor/path) stay enabled, mirroring Recording.
            for (HWND h : {g.comboMonitor, g.comboFps, g.comboBitrate, g.chkGdi})
                EnableWindow(h, FALSE);
            EnableWindow(g.btnStart, FALSE);  // gray out while preparing
            EnableWindow(g.btnPause, FALSE);
            setStatus(UiState::Preparing);    // blue dot/icon + 准备中
            updateTitle();
            UpdateWindow(g.wnd);
        }
        if (quitPosted) break;
    }
    WaitForSingleObject(starter, INFINITE);
    CloseHandle(starter);
    CloseHandle(done);
    ok = rawCtx->ok;
    err = rawCtx->err;

    if (quitPosted) {
        if (ok) {
            g.sess = std::move(sess);
            g.sess->requestStop();
        }
        trayHide();
        g.state = UiState::Ready;
        PostQuitMessage(static_cast<int>(0));
        return;
    }
    if (!ok) {
        trayHide();      // no-op when the tray was never shown
        setUiIdle();     // re-enables controls (safe even if never disabled)
        setStatus(UiState::Ready);
        showError(err);
        if (g.quitting) {
            g.quitting = false;
            DestroyWindow(g.wnd);
        }
        return;
    }
    g.sess = std::move(sess);
    // Quit requested while preparing: stop right away; handleFinished
    // destroys the window once saving completes.
    if (g.quitting) g.sess->requestStop();
    setUiRecording();
    // Options may have been toggled during Preparing (controls stay enabled):
    // sync the live cursor switch; an edited path moves the file on stop.
    g.sess->setDrawCursor(Button_GetCheck(g.chkCursor) == BST_CHECKED);
    setStatus(UiState::Recording);  // shows the tray as a side effect
    updateTitle();
    blinkRecordingStart();
    createBorderWindow();
}

void doStop() {
    if (!g.sess || g.state == UiState::Saving) return;
    g.sess->requestStop();
    g.state = UiState::Saving;  // logical; visuals deferred by 10 ms
    SetTimer(g.wnd, TIMER_SAVING, kTransientStateMs, nullptr);
    // Switch the fps readout to the average for 保存中/保存成功.
    updateStatsLabels(g.sess->statFps.load(), g.sess->statAvgFps.load(),
                      g.sess->statBytes.load(),
                      static_cast<double>(g.sess->statActiveNs.load()) / 1e9, true);
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
