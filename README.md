# Recording — Windows 屏幕录制程序

<img src="icon.svg" width="128" height="128" style="float: right" alt="Recording icon"/>

零外部依赖的 Windows 屏幕录制工具，仅使用系统原生 API（DXGI / GDI / Media Foundation / Win32），
原生 Win32 图形界面 + 系统托盘。构建产物为**单文件 exe**（运行库全部静态链接，无需任何 DLL）。

## 特性

- **采集**：优先 DXGI Desktop Duplication（GPU 高效路径），不可用时自动回退 GDI BitBlt；可强制 GDI
- **编码**：Media Foundation H.264 → MP4（硬件编码可用时自动启用）
- **鼠标指针**：合成到画面中（DXGI 路径为逐像素 alpha 混合，GDI 路径用 DrawIconEx），可关闭
- **多显示器**：选择单个显示器，或录制整个虚拟桌面（GDI 后端）
- **录制边框**：录制时在采集区域四周显示红色边框（暂停变黄），置顶且鼠标穿透，
  通过 `WDA_EXCLUDEFROMCAPTURE` 排除捕获——边框不会录进视频（Windows 10 2004+）
- **实时统计**：信息栏四列（状态/时长/fps/大小，MiB 与资源管理器一致）实时更新，标题栏同步显示
  `(录制中) 屏幕录制 00:00:21 31.0 fps`；暂停及结束后显示平均帧率
- **状态图标**：标题栏/任务栏图标随状态变色（就绪绿、录制红、暂停黄、准备中与保存中蓝），
  图标为程序化预生成缓存（SDF + 超采样抗锯齿），切换零开销；录制中任务栏按钮显示带灰色细环的
  红点角标并以约 2.4s 周期闪烁（红↔暗红，无渐变）
- **系统托盘**：准备/录制/暂停/保存期间常驻——左键单击暂停/继续、双击显示界面、
  中键停止、右键菜单（显示界面/暂停继续/停止）；托盘图标随状态变色并同步闪烁；
  期间关闭主窗口为隐藏（不退出），停止后自动弹出并激活主窗口
- **瞬态抑制**：准备/保存短于 10ms 时不做任何状态视觉变更（按钮/圆点/图标/托盘/标题均不切换）
- **路径模板**：输出路径支持已知文件夹令牌 `CAPTURES`（默认，捕获文件夹）、`VIDEOS`、`PICTURES`、
  `SCREENSHOTS`，以及逐个日期时间令牌 `YYYY`/`YY`/`MM`/`DD`/`HH`/`MM`/`SS`（第一个 MM 为月、
  紧跟 HH 的 MM 为分钟），保存时刻展开为实际路径
- **设置记忆**：输出目录、显示器、帧率、码率、鼠标、后端等选项持久化，任何更改立即保存，重启自动恢复
- **自动建目录**：输出目录不存在时自动创建

## 构建

需要 Windows 10+。CLion 直接打开即可构建；命令行：

```
cmake -S . -B build
cmake --build build
```

MSVC 与 MinGW-w64 均可编译，产出 `RecordingGui.exe`（Windows GUI 子系统，无控制台窗口，
**单文件即可运行**——线程/互斥用 Win32 原生实现，pthread 符号由 `src/pthreadshim.cpp` 垫片满足，
不依赖 `libwinpthread-1.dll`）。

推送 `v*` 标签时 GitHub Actions 自动构建 Release（MinGW-w64 + `-O3` + 符号剥离），
打包 `Recording-<tag>-win64.zip`（单文件 exe + 精简版 README + LICENSE）并发布 Release；
含连字符的标签（`-test`/`-rc1`/`-beta`）发布为预发布版。

## 许可证

[GNU General Public License v3.0](LICENSE)

## 用法

- 顶部设置区：显示器 / 帧率（10–60）/ 码率（自动或 4–40 Mbps）/ 输出路径（浏览选择）/ 鼠标开关 / GDI 兼容后端
- **开始**（默认按钮，Enter 触发）→ 状态点变红、采集区域出现红色边框；**暂停 / 继续**（边框变黄）；
  同一按钮变**停止**，收尾后显示「保存成功」
- 录制中可最小化/关闭主窗口（隐藏到托盘），通过托盘控制录制
- 默认输出模板 `CAPTURES\Capture-YYYYMMDD-HHMMSS.mp4`（捕获文件夹），点击开始时展开为实际日期时间；
  亦可自定义如 `VIDEOS\Rec-YY-MM-DD_HH-MM-SS.mp4`
- 设置保存在 `%APPDATA%\Recording\settings.ini`

## 结构

| 文件                            | 说明                                                                |
|---------------------------------|---------------------------------------------------------------------|
| `src/recorder.h/.cpp`           | 录制会话核心：工作线程、节奏控制、暂停/停止、实时统计              |
| `src/capture.h` + `capture_*.cpp` | 采集：monitors（枚举）/cursor（指针合成）/gdi/dxgi 四个实现单元   |
| `src/fsutil.h/.cpp`             | Win32 路径助手（存在/大小/递归建目录/父路径，无 `<filesystem>`）   |
| `src/pathtokens.h/.cpp`         | 路径模板展开（已知文件夹令牌 + 逐个日期时间令牌）                  |
| `src/encoder.h/.cpp`            | Media Foundation H.264 Sink Writer 封装与实时字节统计             |
| `src/gui.cpp`                   | GUI 主文件：全局/助手、状态机与信息栏、`wndProc`、`wWinMain`       |
| `src/border.cpp`                | 红色录制边框（四条边条窗口，防任务栏自动隐藏）                     |
| `src/controls.cpp`              | 控件布局、组合框填充、路径浏览对话框                                |
| `src/flow.cpp`                  | 录制流程：启动（后台初始化+瞬态抑制）/停止/轮询/收尾               |
| `src/icons.cpp`                 | 状态图标/角标（预生成缓存、录制闪烁）、任务栏角标、抗锯齿状态圆点  |
| `src/tray.cpp`                  | 系统托盘：单击/双击/中键/右键菜单交互，命令经 `WM_COMMAND` 路由    |
| `src/pthreadshim.cpp`           | pthread 符号垫片（SRWLOCK/FLS 实现），使 exe 不依赖 winpthread DLL |
| `src/settings.cpp`              | 设置持久化与 UI → 录制参数映射                                     |
| `src/gui_internal.h`            | GUI 内部共享头：`App`/`UiState`、共享声明、组合框索引常量          |
| `src/resource.h` / `src/gui.rc` | 字符串表（全部 UI 文案，**翻译只需改 gui.rc**）与公共控件 v6 清单  |
| `tools/e2e.ps1`                 | E2E 回归套件（重构安全网）                                         |

详细架构（模块地图、数据流、GUI 状态机、线程模型、渲染性能、MinGW 陷阱清单）见
[ARCHITECTURE.md](ARCHITECTURE.md)；历史问题与实现记录见
[Fixed Bugs&Todos.md](Fixed%20Bugs%26Todos.md)。

## 实现说明

- DXGI 后端在会话锁定/UAC 安全桌面等场景下自动重建；长时间无屏幕更新时复制上一帧以保持恒定帧率
- 每帧显示时长按实际采集间隔计算（钳制在 0.5–10 帧周期），保证回放时长与真实录制时长一致
- 文件大小统计优先取编码器实时字节数（`IMFSinkWriter::GetStatistics`），结束时以磁盘实际大小为准
- 会话初始化在后台线程执行、主线程持续泵消息：UI 不冻结，且初始化快于 10ms 时整个「准备中」状态不出现
