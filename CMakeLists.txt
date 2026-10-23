cmake_minimum_required(VERSION 4.3)
project(Recording LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared recording core: capture backends + H.264 encoder + session control.
add_library(recorder_core STATIC
    src/recorder.cpp
    src/fsutil.cpp
    src/pathtokens.cpp
    src/capture_monitors.cpp
    src/capture_cursor.cpp
    src/capture_gdi.cpp
    src/capture_dxgi.cpp
    src/encoder.cpp
)
target_include_directories(recorder_core PUBLIC src)
target_compile_definitions(recorder_core PUBLIC
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_link_libraries(recorder_core PUBLIC
    user32 gdi32 dxgi d3d11 mfplat mfreadwrite mfuuid ole32 shell32 uuid
)
if(MSVC)
    target_compile_options(recorder_core PRIVATE /utf-8)
endif()

# GUI front-end (Win32, common controls v6 via embedded manifest).
add_executable(RecordingGui WIN32
    src/gui.cpp
    src/border.cpp
    src/controls.cpp
    src/flow.cpp
    src/icons.cpp
    src/settings.cpp
    src/tray.cpp
    src/pthreadshim.cpp
    src/gui.rc
)
target_link_libraries(RecordingGui PRIVATE recorder_core comctl32 comdlg32)
if(NOT MSVC)
    target_compile_options(RecordingGui PRIVATE -municode)
    target_link_options(RecordingGui PRIVATE -municode)
endif()

if(NOT MSVC)
    # Static-link the C/C++ runtimes; pthread symbols needed by libstdc++
    # are satisfied by src/pthreadshim.cpp (Win32), so the exe is fully
    # self-contained - no libwinpthread-1.dll required.
    target_link_options(RecordingGui PRIVATE -static-libgcc -static-libstdc++)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        # Strip symbols for a smaller release binary.
        target_link_options(RecordingGui PRIVATE -s)
    endif()
endif()
