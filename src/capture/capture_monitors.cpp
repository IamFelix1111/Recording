// Monitor enumeration (primary first, stable indices for the UI combo).

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "capture.h"
#include "capture_internal.h"

std::string wideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::vector<MonitorInfo> listMonitors() {
    std::vector<MonitorInfo> mons;
    mons.reserve(4);
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hmon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hmon, &mi)) {
                MonitorInfo m;
                m.index = static_cast<int>(out->size());
                m.x = mi.rcMonitor.left;
                m.y = mi.rcMonitor.top;
                m.width = mi.rcMonitor.right - mi.rcMonitor.left;
                m.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
                m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                m.device = wideToUtf8(mi.szDevice);
                m.handle = hmon;
                out->push_back(m);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&mons));
    if (!mons.empty() && !mons[0].primary) {
        // Put the primary monitor first so index 0 is predictable.
        auto it = std::ranges::find_if(
            mons, [](const MonitorInfo& m) { return m.primary; });
        if (it != mons.end()) std::swap(mons[0], *it);
        for (size_t i = 0; i < mons.size(); ++i) mons[i].index = static_cast<int>(i);
    }
    return mons;
}
