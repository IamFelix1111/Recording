# Recording — Windows 屏幕录制程序 (C++)

零外部依赖的 Windows 屏幕录制工具，仅使用系统原生 API（DXGI / GDI / Media Foundation / Win32），
原生 Win32 图形界面。

## 特性

- **采集**：优先 DXGI Desktop Duplication（GPU 高效路径），不可用时自动回退 GDI BitBlt；可强制 GDI
- **编码**：Media Foundation H.264 → MP4（硬件编码可用时自动启用）
- **鼠标指针**：合成到画面中（DXGI 路径为逐像素 alpha 混合，GDI 路径用 DrawIconEx），可关闭
- **多显示器**：选择单个显示器，或录制整个虚拟桌面（GDI 后端）
- **录制边框**：录制时在采集区域四周显示红色边框（暂停变黄），置顶且鼠标穿透，
  通过 `WDA_EXCLUDEFROMCAPTURE` 排除捕获——边框不会录进视频（Windows 10 2004+）
- **实时统计**：时长 / 帧数 / fps / 文件大小（MiB，与资源管理器一致）实时更新，标题栏同步显示录制状态
- **状态图标**：标题栏与任务栏图标随状态变色（就绪绿/录制红/暂停黄/准备中与保存中灰），录制中任务栏红点缓慢闪烁；
  图标为程序化预生成缓存（SDF + 超采样抗锯齿），切换零开销，低配设备亦流畅
- **设置记忆**：输出目录、显示器、帧率、码率、鼠标、后端等选项持久化，重启后自动恢复
- **自动建目录**：输出目录不存在时自动创建

## 构建

需要 Windows 10+。CLion 直接打开即可构建；命令行：

```
cmake -S . -B build
cmake --build build
```

MSVC 与 MinGW-w64 均可编译，产出 `RecordingGui.exe`（Windows GUI 子系统，无控制台窗口）。

## 许可证

[GNU General Public License v3.0](LICENSE)

## 用法（RecordingGui.exe）

- 顶部设置区：显示器 / 帧率（10–60）/ 码率（自动或 4–40 Mbps）/ 输出路径（浏览选择）/ 鼠标开关 / GDI 兼容后端
- **开始** → 状态点变红、采集区域出现红色边框；**暂停 / 继续**（边框变黄）；同一按钮变**停止**，收尾后显示「保存成功」
- 录制中实时显示时长、帧数、fps、文件大小；标题栏显示 `(录制中) 屏幕录制 00:00:21 31.0 fps`
- 设置保存在 `%APPDATA%\Recording\settings.ini`，任何配置更改立即保存，下次启动自动恢复
- 默认输出到「视频」文件夹，路径框显示模板 `Recording-YYYYMMDD-HHMMSS.mp4`，
  点击开始时替换为实际日期时间

## 结构

| 文件                            | 说明                                                              |
|---------------------------------|-------------------------------------------------------------------|
| `src/recorder.h/.cpp`           | 录制会话核心：工作线程、节奏控制、暂停/停止、实时统计             |
| `src/capture.h/.cpp`            | 采集后端（DXGI / GDI）、屏幕区域信息与鼠标指针合成                |
| `src/encoder.h/.cpp`            | Media Foundation H.264 Sink Writer 封装与实时字节统计             |
| `src/gui.cpp`                   | GUI 主文件：窗口生命周期、控件布局、录制流程、红色录制边框        |
| `src/icons.cpp`                 | 状态图标（预生成缓存、录制闪烁）与抗锯齿状态圆点                  |
| `src/settings.cpp`              | 设置持久化与 UI → 录制参数映射                                    |
| `src/gui_internal.h`            | GUI 内部共享头：`App`/`UiState`、共享声明、组合框索引常量         |
| `src/resource.h` / `src/gui.rc` | 字符串表（全部 UI 文案，**翻译只需改 gui.rc**）与公共控件 v6 清单 |

详细架构（模块地图、数据流、GUI 状态机、线程模型、MinGW 陷阱清单）见 [ARCHITECTURE.md](ARCHITECTURE.md)。

## 实现说明

- DXGI 后端在会话锁定/UAC 安全桌面等场景下自动重建；长时间无屏幕更新时复制上一帧以保持恒定帧率
- 每帧显示时长按实际采集间隔计算（钳制在 0.5–10 帧周期），保证回放时长与真实录制时长一致
- 文件大小统计优先取编码器实时字节数（`IMFSinkWriter::GetStatistics`），结束时以磁盘实际大小为准
