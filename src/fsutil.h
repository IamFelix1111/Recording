// Win32 path helpers (kept <filesystem>-free so libstdc++ locale/EH glue
// that drags in winpthreads is not linked).
#pragma once

#include <cstdint>
#include <string>

bool dirExists(const std::wstring& path);
std::uint64_t fileSizeOf(const std::wstring& path);  // 0 on failure
bool ensureDirPath(const std::wstring& dir);         // recursive CreateDirectoryW
std::wstring parentPathOf(const std::wstring& path);
std::wstring fileNameOf(const std::wstring& path);
