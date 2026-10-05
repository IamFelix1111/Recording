cmake_minimum_required(VERSION 4.3)
project(Recording LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared recording core: capture backends + H.264 encoder + session control.
add_library(recorder_core STATIC
    src/recorder.cpp
    src/capture.cpp
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
    src/gui.rc
)
target_link_libraries(RecordingGui PRIVATE recorder_core comctl32 comdlg32)
if(NOT MSVC)
    target_compile_options(RecordingGui PRIVATE -municode)
    target_link_options(RecordingGui PRIVATE -municode)
endif()

if(NOT MSVC)
    # Static-link the C/C++ runtimes so the exe needs only libwinpthread-1.dll,
    # which is copied next to the binary below.
    target_link_options(RecordingGui PRIVATE -static-libgcc -static-libstdc++)
    get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET RecordingGui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_mingw_bin}/libwinpthread-1.dll" $<TARGET_FILE_DIR:RecordingGui>)
endif()
