// Settings persistence (%APPDATA%\Recording\settings.ini) and the
// UI -> RecorderSettings mapping. Combo index conventions live in
// gui_internal.h next to kMonitorSel*/kFps*/kBitrate*.

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>

#include <cstdio>
#include <filesystem>

#include "gui_internal.h"

RecorderSettings settingsFromUi() {
    RecorderSettings s;
    const int monSel = ComboBox_GetCurSel(g.comboMonitor);
    s.allMonitors = (monSel <= kMonitorSelAllMonitors);
    s.monitorIndex =
        monSel > kMonitorSelAllMonitors ? monSel - kMonitorSelFirstMonitor : 0;
    const int fpsSel = ComboBox_GetCurSel(g.comboFps);
    s.fps = (fpsSel >= 0 && fpsSel < kFpsChoiceCount) ? kFpsChoices[fpsSel]
                                                      : kFpsChoices[kFpsDefaultSel];
    s.bitrateBps = static_cast<std::uint64_t>(
        ComboBox_GetItemData(g.comboBitrate, ComboBox_GetCurSel(g.comboBitrate)));
    s.outputPath = windowText(g.editPath);
    if (s.outputPath.empty()) s.outputPath = defaultRecordingTemplate();
    // Resolve Recording-YYYYMMDD-HHMMSS.mp4 to the actual save moment.
    s.outputPath = expandRecordingPath(s.outputPath);
    if (s.outputPath.find_last_of(L'.') == std::wstring::npos ||
        s.outputPath.find_last_of(L'.') < s.outputPath.find_last_of(L'\\')) {
        s.outputPath += L".mp4";  // no extension: add one
    }
    s.drawCursor = (Button_GetCheck(g.chkCursor) == BST_CHECKED);
    s.preferGdi = (Button_GetCheck(g.chkGdi) == BST_CHECKED);
    return s;
}

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
    swprintf(buf, 16, L"%d", ComboBox_GetCurSel(g.comboFps));
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
    g.loading = true;  // programmatic changes must not re-save/clobber
    wchar_t dir[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"Settings", L"OutputDir", L"", dir, MAX_PATH,
                             ini.c_str());
    if (dir[0] && std::filesystem::is_directory(dir)) {
        // Remembered folder + fresh Recording-YYYYMMDD-HHMMSS.mp4 template.
        std::filesystem::path p = defaultRecordingTemplate();
        std::wstring path = std::wstring(dir) + L"\\" + p.filename().wstring();
        SetWindowTextW(g.editPath, path.c_str());
    }
    const int monSel =
        static_cast<int>(GetPrivateProfileIntW(L"Settings", L"MonitorSel", -1, ini.c_str()));
    if (monSel >= 0 && monSel < ComboBox_GetCount(g.comboMonitor))
        ComboBox_SetCurSel(g.comboMonitor, monSel);
    const int fpsSel =
        static_cast<int>(GetPrivateProfileIntW(L"Settings", L"FpsSel", -1, ini.c_str()));
    if (fpsSel >= 0 && fpsSel < ComboBox_GetCount(g.comboFps))
        ComboBox_SetCurSel(g.comboFps, fpsSel);
    const int brSel =
        static_cast<int>(GetPrivateProfileIntW(L"Settings", L"BitrateSel", -1, ini.c_str()));
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
    g.loading = false;
}
