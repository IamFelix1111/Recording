# Recording — Windows 屏幕录制程序 (C++)

零外部依赖的 Windows 屏幕录制工具，仅使用系统原生 API（DXGI / GDI / Media Foundation / Win32）。
提供 **GUI** 与 **命令行** 两个前端，共用同一录制核心。

## 特性

- **采集**：优先 DXGI Desktop Duplication（GPU 高效路径），不可用时自动回退 GDI BitBlt；可强制 GDI
- **编码**：Media Foundation H.264 → MP4（硬件编码可用时自动启用）
- **鼠标指针**：合成到画面中（DXGI 路径为逐像素 alpha 混合，GDI 路径用 DrawIconEx），可关闭
- **多显示器**：选择单个显示器，或录制整个虚拟桌面（GDI 后端）
- **录制边框**（GUI）：录制时在采集区域四周显示红色边框（暂停变黄），置顶且鼠标穿透，
  通过 `WDA_EXCLUDEFROMCAPTURE` 排除捕获——边框不会录进视频（Windows 10 2004+）
- **实时统计**：时长 / 帧数 / fps / 文件大小（MiB，与资源管理器一致）实时更新
- **设置记忆**（GUI）：输出目录、显示器、帧率、码率、鼠标、后端等选项持久化，重启后自动恢复
- **自动建目录**：输出目录不存在时自动创建

## 构建

需要 Windows 10+。CLion 直接打开即可构建；命令行：

```
cmake -S . -B build
cmake --build build
```

MSVC 与 MinGW-w64 均可编译。产出两个可执行文件：

| 目标               | 说明                          |
|--------------------|-------------------------------|
| `Recording.exe`    | 命令行前端（控制台程序）      |
| `RecordingGui.exe` | 图形界面前端（Win32 原生 UI） |

## GUI 用法（RecordingGui.exe）

- 顶部设置区：显示器 / 帧率（10–60）/ 码率（自动或 4–40 Mbps）/ 输出路径（浏览选择）/ 鼠标开关 / GDI 兼容后端
- **开始录制** → 状态点变红、采集区域出现红色边框；**暂停 / 继续**（边框变黄）；**停止** 后自动收尾保存
- 录制中实时显示时长、帧数、fps、文件大小
- 设置保存在 `%APPDATA%\Recording\settings.ini`，下次启动自动恢复
- 默认输出到「视频」文件夹（`recording_<时间戳>.mp4`）

## CLI 用法（Recording.exe）

```
Recording.exe [选项]
  -o, --output <file>      输出 MP4 文件（默认「视频」文件夹 recording_<时间戳>.mp4）
  -f, --fps <n>            帧率 1..60（默认 30）
  -m, --monitor <n>        显示器索引，见 --list（默认 0 = 主显示器）
  -a, --all-monitors       录制整个虚拟桌面（GDI 后端）
  -t, --time <seconds>     n 秒后自动停止
      --bitrate <kbps>     视频码率 kbps（默认按分辨率自动）
      --no-cursor          不绘制鼠标指针
      --gdi                强制 GDI 采集后端
      --list               列出显示器并退出
  -h, --help               帮助
```

示例：

```
Recording.exe -t 60 -o demo.mp4          # 录 60 秒
Recording.exe -f 60 --bitrate 20000      # 60fps 高码率
Recording.exe -m 1 --no-cursor           # 第二台显示器，不含鼠标
```

录制中热键：**F9** 暂停/恢复，**F10** 或 **Ctrl+C** 停止。

## 结构

| 文件                              | 说明                                                  |
|-----------------------------------|-------------------------------------------------------|
| `src/recorder.h/.cpp`             | 录制会话核心：工作线程、节奏控制、暂停/停止、实时统计 |
| `src/capture.h/.cpp`              | 采集后端（DXGI / GDI）、屏幕区域信息与鼠标指针合成    |
| `src/encoder.h/.cpp`              | Media Foundation H.264 Sink Writer 封装与实时字节统计 |
| `src/main.cpp`                    | CLI 前端：参数解析、热键、状态行输出                  |
| `src/gui.cpp`                     | GUI 前端：Win32 控件、红色录制边框、设置持久化        |
| `src/gui.rc` / `src/gui.manifest` | GUI 资源与公共控件 v6 视觉样式清单                    |

## 实现说明

- DXGI 后端在会话锁定/UAC 安全桌面等场景下自动重建；长时间无屏幕更新时复制上一帧以保持恒定帧率
- 每帧显示时长按实际采集间隔计算（钳制在 0.5–10 帧周期），保证回放时长与真实录制时长一致
- 文件大小统计优先取编码器实时字节数（`IMFSinkWriter::GetStatistics`），结束时以磁盘实际大小为准
