// Internal helpers shared between the capture translation units
// (capture_monitors / capture_cursor / capture_gdi / capture_dxgi).
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// UTF-8 conversion of a device name (monitor enumeration).
std::string wideToUtf8(const wchar_t* w);

// "0x%08lX" HRESULT formatting for error messages.
std::string hrText(HRESULT hr);

// Composites the mouse cursor onto a top-down BGRA frame (DXGI backend;
// GDI uses DrawIconEx into its memory DC instead).
void overlayCursor(std::uint8_t* px, int w, int h, int frameOriginX,
                   int frameOriginY);
