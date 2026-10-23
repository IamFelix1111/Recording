// Output-path templates: known-folder root tokens (CAPTURES/VIDEOS/...)
// and per-token date/time expansion (YYYY/YY/MM/DD/HH/MM/SS).
#pragma once

#include <string>

// Token-based and user-portable: "CAPTURES\Capture-YYYYMMDD-HHMMSS.mp4".
std::wstring defaultRecordingTemplate();
// Expands root + date/time tokens with the values at call time.
std::wstring expandRecordingPath(const std::wstring& path);
