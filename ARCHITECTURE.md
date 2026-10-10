# ARCHITECTURE — Recording

> 面向 Agent / 新维护者的项目结构速览。改动前先读此文件。

## 总览

Windows 屏幕录制器，零第三方依赖。录制核心库 + GUI 前端：

```
cmake-build-debug/
├── recorder_core (static lib)   ← 录制核心（无 UI）
└── RecordingGui.exe             ← GUI 前端（Win32, GUI 子系统，无控制台）
```

## 模块地图

| 文件                            | 角色           | 关键符号                                                                                                                                                                                                                                                         |
|---------------------------------|----------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `src/capture.h/.cpp`            | 屏幕采集       | `IScreenCapture`（接口：captureFrame/screenRect）、`DxgiCapture`（DXGI Desktop Duplication）、`GdiCapture`（BitBlt 回退/多屏）、`listMonitors()`、`overlayCursor()`（BGRA 上逐像素 alpha 混合鼠标）                                                              |
| `src/encoder.h/.cpp`            | 视频编码       | `VideoEncoder`：MF Sink Writer，输入 RGB32 → H.264/MP4；`bytesWritten()` 实时字节数（`GetStatistics`）                                                                                                                                                           |
| `src/recorder.h/.cpp`           | 会话编排       | `RecorderSession`：start/请求暂停/请求停止，工作线程跑 `runLoop`（帧率节奏 + 钳制帧时长），原子统计量（statFrames/statActiveNs/statBytes/statFps/statAvgFps）；`autoBitrate()`、`defaultRecordingTemplate()`/`expandRecordingPath()`（YYYYMMDD-HHMMSS 模板展开） |
| `src/gui.cpp`                   | GUI 主文件     | 窗口生命周期、控件布局（`createControls`/`fillCombos`）、录制流程（onStart/doStop/onPoll）、红色录制边框（`borderProc`）、wWinMain                                                                                                                               |
| `src/icons.cpp`                 | 状态图标       | `stateColor`（状态色唯一来源）、`makeStateIcon`（SDF+3x3 超采样程序化图标）、5 桶预生成缓存（含 `kIconBucketRedDim` 闪烁暗红）、`updateStateIcons`/`onBlinkTick`（录制中任务栏红/暗红 ≈1.2s 相位交替）、`drawDotAntialiased`                                     |
| `src/settings.cpp`              | 设置持久化     | `settingsFromUi`（UI→RecorderSettings）、`saveUiSettings`/`loadUiSettings`（%APPDATA%\Recording\settings.ini）                                                                                                                                                   |
| `src/gui_internal.h`            | GUI 内部共享头 | App 结构（含图标缓存/脏检查缓存）、`UiState`、`kIconBucket*`、跨文件函数声明、**组合框索引常量**（kMonitorSel*/kFps*/kBitrate*，索引含义唯一定义处）                                                                                                             |
| `src/tray.cpp`                  | 托盘图标       | `Shell_NotifyIconW` 生命周期（Preparing/Recording/Paused/Saving 可见）；左键单击暂停（双击防误触）、双击显示、中键停止、右键菜单，命令经 `IDM_TRAY_*` 走 `WM_COMMAND`                                                                                            |
| `src/pthreadshim.cpp`           | pthread 垫片   | 以 SRWLOCK/Interlocked/FLS 实现 libstdc++ 引用的 8 个 pthread 符号（once/mutex×4/key×3），使 exe 不依赖 `libwinpthread-1.dll`（单文件）                                                                                              |
| `src/resource.h` + `src/gui.rc` | 字符串表       | 全部 UI 文案（`IDS_*`），翻译只需改 gui.rc 的 STRINGTABLE                                                                                                                                                                                                        |
| `src/gui.manifest`              | 清单           | 公共控件 v6 视觉样式                                                                                                                                                                                                                                             |

## 数据流

```
IScreenCapture::captureFrame()   →  VideoFrame{BGRA, w, h}
        │ (工作线程, 每 1/fps)
        ▼
VideoEncoder::writeFrame()       →  IMFSinkWriter → MP4 (H.264)
        │
        ▼
原子统计 ← UI 200ms 轮询 (onPoll)
```

## GUI 状态机（gui.cpp）

```
Ready(绿/就绪) --开始--> Preparing(灰/准备中) --ok--> Recording(红) <--> Paused(黄)
              (按钮变灰)          |失败→ Ready
Recording/Paused --停止/关窗--> Saving(灰/保存中) --worker 结束--> Ready(保存成功)
```
- 主按钮双态：`开始`（idle）/`停止`（recording），无独立停止钮；Preparing 期间禁用防重入（state==Ready 守卫）
- 标题栏全状态感知：`(录制中|暂停) 屏幕录制 hh:mm:ss N.N fps`（实际值，`updateTitle()`），
  准备中/保存中显示 `(准备中|保存中) 屏幕录制`，Ready 复原为 `屏幕录制`
- `onPoll` 在 `Saving` 态直接返回，避免覆盖"保存中"

## 线程模型

- **UI 线程**：消息循环 + 200ms TIMER_POLL；`RecorderSession::start()` 阻塞式初始化（约 100–500ms，之前先画"准备中"）
- **工作线程**（RecorderSession::worker）：COM(MTA) + 采集 + 编码 + 统计；结束发 `WM_APP_FINISHED`（`onFinished` 回调 PostMessage）
- 停止：`requestStop()` 置原子标志 → runLoop 退出 → encoder.finish → `running_=false`

## 渲染性能

- 状态图标：启动时 5 色 × 大小一次性预生成（`ensureStateIcons`），切换仅 `WM_SETICON` 换句柄，零重生成
- 录制闪烁：`onBlinkTick` 在 200ms 轮询里按 `GetTickCount64()/1200` 相位切任务栏大图标（红↔暗红），
  仅相位变化时发送消息；标题栏小图标不闪。任务栏对快速换图标会合并重绘，故用慢速相位
- 状态圆点/边框：事件驱动重绘（仅状态变化），无持续开销
- 标签脏检查：`setTextIfChanged`——onPoll 每 200ms 更新标题与信息列，文本未变时跳过 `WM_SETTEXT`

## 持久化

- `%APPDATA%\Recording\settings.ini`：OutputDir/MonitorSel/FpsSel/BitrateSel/DrawCursor/PreferGdi
- **任何配置更改立即写入**（WM_COMMAND 监听 CBN_SELCHANGE/BN_CLICKED/EN_CHANGE → `saveUiSettings`）
- `g.loading` 标志：启动恢复（loadUiSettings 播种控件）期间抑制程序化设值触发的回写，防止默认值覆盖已存配置

## 平台 / 工具链注意事项（MinGW-w64 陷阱）

- 宽字符 `swprintf` 中 `%s`=窄串，宽串必须用 **`%ls`**（MSVC 恰相反）
- `mfreadwrite.h` 需先 include `mfidl.h`（IMFMediaSink）；`MF_SINK_WRITER_STATISTICS` 字段名为 `cb`/`qwByteCountProcessed`（MSVC 是 `cbSize`/`qwByteCount`），encoder.cpp 有 `__MINGW32__` 条件编译
- `AddStream()` 设置输出格式（无 `SetOutputMediaType`）；无 `MFCreate2DMemoryBuffer`（用平面 buffer + memcpy）
- 静态链接：`-static-libgcc -static-libstdc++`；线程/互斥为 Win32 原生（`_beginthreadex`/`SRWLOCK`/Event），
  libstdc++ 残留的 pthread 引用由 `src/pthreadshim.cpp` 垫片满足 → **单文件 exe，无 `libwinpthread-1.dll`**
- gui.rc：UTF-8 编码 + `#pragma code_page(65001)`；GUI 目标需 `-municode`

## 已知边界

- 红色边框用 `WDA_EXCLUDEFROMCAPTURE`（Win10 2004+；更旧系统边框会被录进视频）
- DXGI 在锁屏/UAC 桌面会 ACCESS_LOST → 自动重建；无屏幕更新时复制上一帧
- “所有显示器”仅 GDI 后端支持（DXGI 每输出一个 duplication）
