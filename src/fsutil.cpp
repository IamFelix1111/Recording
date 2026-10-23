#include "fsutil.h"

#include <windows.h>

bool dirExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::uint64_t fileSizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return 0;
    return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
           info.nFileSizeLow;
}

bool ensureDirPath(const std::wstring& dir) {
    if (dir.empty() || dirExists(dir)) return true;
    std::wstring prefix;
    int seps = 0;
    for (size_t i = 0; i < dir.size(); ++i) {
        const wchar_t c = dir[i];
        if (c == L'\\' || c == L'/') {
            const bool uncLead = !prefix.empty() && prefix[0] == L'\\' && seps < 3;
            // Create the prefix for real components only: skip drive roots
            // ("C:" / "C:\") and UNC leads ("\\server\share").
            if (prefix.size() > 2 && !uncLead) {
                if (!CreateDirectoryW(prefix.c_str(), nullptr) &&
                    GetLastError() != ERROR_ALREADY_EXISTS)
                    return false;
            }
            ++seps;
        }
        prefix += c;
    }
    if (prefix.size() > 2 && !(prefix[0] == L'\\' && seps < 4)) {
        if (!CreateDirectoryW(prefix.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    return true;
}

std::wstring parentPathOf(const std::wstring& path) {
    const size_t n = path.find_last_of(L"\\/");
    if (n == std::wstring::npos || n < 3) return L"";  // no parent / drive root
    return path.substr(0, n);
}

std::wstring fileNameOf(const std::wstring& path) {
    const size_t n = path.find_last_of(L"\\/");
    return n == std::wstring::npos ? path : path.substr(n + 1);
}
