// Control layout and combo-box population; output-path "Browse..." dialog.

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>

#include <cstdio>

#include "gui_internal.h"
#include "fsutil.h"
#include "pathtokens.h"

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

    ComboBox_AddString(g.comboApi, loadStr(IDS_API_DXGI).c_str());
    ComboBox_AddString(g.comboApi, loadStr(IDS_API_GDI).c_str());
    ComboBox_SetCurSel(g.comboApi, kApiSelDxgi);
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
    g.lblApi = createCtl(L"STATIC", loadStr(IDS_LBL_API).c_str(), SS_CENTERIMAGE,
                         210, 52, 34, 22, 0);
    g.comboApi = createCtl(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_TABSTOP,
                           248, 52, 128, 200, IDC_API);

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
                   g.comboBitrate, g.lblApi, g.comboApi, g.lblPath, g.editPath,
                   g.btnBrowse, g.chkCursor, g.stDot, g.stStatus, g.stTime, g.stFps,
                   g.stSize, g.btnStart, g.btnPause})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
}
