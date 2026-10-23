// Output-path template expansion: known-folder root tokens
// (CAPTURES/VIDEOS/PICTURES/SCREENSHOTS) and per-token date/time
// (YYYY/YY/MM/DD/HH/MM/SS; the MM right after HH is minutes).

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <cwchar>
#include <cwctype>
#include <ctime>
#include <utility>

#include "pathtokens.h"

namespace {

// Expands date/time tokens left-to-right: YYYY YY MM DD HH SS; MM right
// after HH (separators allowed) means minutes, otherwise month. Composite
// forms like YYYYMMDD and HHMMSS fall out naturally: YYYY+MM+DD, HH+MM+SS.
std::wstring expandDateTimeTokens(std::wstring s, const std::tm& tm) {
    wchar_t year[8], yy[4], mon[4], day[4], hour[4], minute[4], sec[4];
    swprintf(year, 8, L"%04d", tm.tm_year + 1900);
    swprintf(yy, 4, L"%02d", (tm.tm_year + 1900) % 100);
    swprintf(mon, 4, L"%02d", tm.tm_mon + 1);
    swprintf(day, 4, L"%02d", tm.tm_mday);
    swprintf(hour, 4, L"%02d", tm.tm_hour);
    swprintf(minute, 4, L"%02d", tm.tm_min);
    swprintf(sec, 4, L"%02d", tm.tm_sec);

    std::wstring out;
    out.reserve(s.size() + 8);
    bool afterHH = false;  // HH matched, separators allowed since
    for (size_t i = 0; i < s.size();) {
        const wchar_t c = s[i];
        if (c == L'Y' && s.compare(i, 4, L"YYYY") == 0) {
            out += year;
            i += 4;
            afterHH = false;
        } else if (c == L'Y' && s.compare(i, 2, L"YY") == 0) {
            out += yy;
            i += 2;
            afterHH = false;
        } else if (c == L'H' && s.compare(i, 2, L"HH") == 0) {
            out += hour;
            i += 2;
            afterHH = true;
        } else if (c == L'D' && s.compare(i, 2, L"DD") == 0) {
            out += day;
            i += 2;
            afterHH = false;
        } else if (c == L'S' && s.compare(i, 2, L"SS") == 0) {
            out += sec;
            i += 2;
            afterHH = false;
        } else if (c == L'M' && s.compare(i, 2, L"MM") == 0) {
            out += afterHH ? minute : mon;
            i += 2;  // MM keeps the HH context (HH-MM-SS stays consistent)
        } else {
            if (iswalnum(c) == 0) {
                // separator: HH context survives ("HH:MM", "HH-MM-SS")
            } else {
                afterHH = false;  // any literal letter/digit resets the context
            }
            out += c;
            ++i;
        }
    }
    return out;
}

// Known-folder GUIDs (FOLDERID_*). Captures/Screenshots are Windows 10
// folders missing from some MinGW-w64 headers, so spelled out here.
constexpr GUID kFolderVideos = {
    0x18989b1d, 0x99b5, 0x455b, {0x84, 0x1c, 0xab, 0x7c, 0x74, 0xe4, 0xdd, 0xfc}};
constexpr GUID kFolderPictures = {
    0x33e28130, 0x4e1e, 0x4676, {0x8d, 0x35, 0xa9, 0x83, 0x95, 0xc3, 0xbc, 0x3b}};
constexpr GUID kFolderCaptures = {
    0xedc0fe71, 0x98d8, 0x4f4a, {0xb9, 0x20, 0xc8, 0xdc, 0x13, 0x3c, 0xb1, 0x65}};
constexpr GUID kFolderScreenshots = {
    0xb7bede81, 0xdf94, 0x4682, {0xa7, 0xd8, 0x57, 0xa5, 0x26, 0x20, 0xb8, 0x6f}};

std::wstring knownFolderPath(const GUID& id) {
    wchar_t* p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
        std::wstring dir = p;
        CoTaskMemFree(p);
        return dir;
    }
    return {};
}

bool iequalsW(const std::wstring& a, const wchar_t* b) {
    return _wcsicmp(a.c_str(), b) == 0;
}

// Expands a leading VIDEOS/CAPTURES/PICTURES/SCREENSHOTS path component to
// its known-folder location. Returns false when the path has no such token.
bool expandRootToken(std::wstring& p) {
    const size_t sep = p.find_first_of(L"\\/");
    const std::wstring root = sep == std::wstring::npos ? p : p.substr(0, sep);
    const std::wstring rest = sep == std::wstring::npos ? L"" : p.substr(sep + 1);

    std::wstring dir;
    if (iequalsW(root, L"VIDEOS")) {
        dir = knownFolderPath(kFolderVideos);
    } else if (iequalsW(root, L"CAPTURES")) {
        // Fallback when the known folder is not registered: <Videos>\Captures.
        dir = knownFolderPath(kFolderCaptures);
        if (dir.empty()) {
            const std::wstring videos = knownFolderPath(kFolderVideos);
            if (!videos.empty()) dir = videos + L"\\Captures";
        }
    } else if (iequalsW(root, L"PICTURES")) {
        dir = knownFolderPath(kFolderPictures);
    } else if (iequalsW(root, L"SCREENSHOTS")) {
        dir = knownFolderPath(kFolderScreenshots);
        if (dir.empty()) {
            const std::wstring pics = knownFolderPath(kFolderPictures);
            if (!pics.empty()) dir = pics + L"\\Screenshots";
        }
    } else {
        return false;
    }
    if (dir.empty()) return false;

    p = rest.empty() ? dir : dir + L"\\" + rest;
    return true;
}

}  // namespace

std::wstring defaultRecordingTemplate() {
    // Token-based and user-portable: CAPTURES expands to the Captures known
    // folder at save time (see expandRecordingPath).
    return L"CAPTURES\\Capture-YYYYMMDD-HHMMSS.mp4";
}

std::wstring expandRecordingPath(const std::wstring& path) {
    std::wstring p = path;
    expandRootToken(p);  // leading known-folder token, if any
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    return expandDateTimeTokens(std::move(p), tm);
}
