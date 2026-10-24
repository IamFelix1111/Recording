# Fixed Bugs
* 双击托盘会自动继续录制
  * Solution: 根因是 shell 双击序列在 `WM_LBUTTONDBLCLK` 之后还发一个尾随 `WM_LBUTTONUP`，它再次 armed `TIMER_TRAYCLICK` 导致 500ms 后 toggle。修复：记录 `g_lastDblClk` 时间戳，`WM_LBUTTONUP` 在双击时限内到达则忽略。实测 UP→DBLCLK→尾随UP 序列后状态不变（仅显示窗口），两向均无误触发
* 应将YYYY渲染为(例如)2026，将YY渲染为26，将MM渲染为10，将DD渲染为10，将HH渲染为17，将第二个MM渲染为30，将SS渲染为00，而非整体渲染YYYYMMDD
  * Solution: `expandDateTimeTokens()` 左到右逐令牌解析 `YYYY/YY/MM/DD/HH/SS`；MM 紧跟 HH（允许分隔符）=分钟，否则=月份（`afterHH` 上下文，字母数字重置）；组合式 `YYYYMMDD`/`HHMMSS` 自然分解为 `YYYY+MM+DD`/`HH+MM+SS`。实测 `T-YY-MM-DD_HH-MM-SS.mp4` → `T-26-10-10_17-45-35.mp4`，默认模板回归 `Capture-20261010-174617.mp4`
* 任务栏图标并没有闪烁，应降低闪烁频率
  * Solution: 任务栏对快速图标切换会合并重绘；相位间隔 500→800ms 后可见，最终定为 1.2s（整周期约 2.4s）。注：shell 忙碌时（开窗等）可能跳过个别帧，属正常。（后该方案整体被任务栏角标取代，见 Fixed Todos）
* 录制时显示的帧率总是.0
  * Solution: `statFps` 原为每秒整数帧数（必为整数）；改为除以真实经过时间（1.00–1.03s 窗口）得带小数 fps；暂停恢复时重置统计窗口。实测 55.1/55.4 fps
* 时长一直是0
* 大小未实时更新
* 准备中时开始按钮没有变灰
  * Solution: `onStart` 在阻塞初始化前 `EnableWindow(btnStart/btnPause, FALSE)`，成功后 `setUiRecording` 恢复为「停止」；另加 `state == Ready` 防重入守卫（后被瞬态延后方案细化）
* 大小不正确
  * Description: 录制完成后，显示文件大小与实际文件大小不正确
  * Solution: 实为十进制 MB（÷1,000,000）与资源管理器 MiB（÷1,048,576）单位不一致；已统一为 MiB，并在结束时直接重读文件实际大小
* 录制鼠标指针选项无效
  * Description: 关闭录制鼠标指针后，鼠标指针仍然会出现在录制的视频中
  * Solution: GDI 后端的 `init` 未接收 `drawCursor` 参数（恒为 `true`）；已传入并在两条路径（DXGI/GDI）生效。另：输出目录不存在时自动创建（修复 `0x80070003`）

# Fixed Todos
* ##### Every Day Check Notes:
  * 2026-10-10: 本轮深审伴随托盘/瞬态实现：新增 `tray.cpp`（交互→`WM_COMMAND` 路由）、`onStart` 改后台线程初始化+消息泵（UI 不再冻结）、发现并修复 `ITaskbarList3` 静态引用未释放导致退出拖延 2–3.6s（`releaseTaskbarCom()`）

* 将 API 复选框改为下拉框（GitHub Issue #1）
  * Solution: 「使用 GDI 兼容后端」`BS_AUTOCHECKBOX`（`IDC_GDI`/`chkGdi`/`IDS_CHK_GDI`）改为「API」`CBS_DROPDOWNLIST`（`IDC_API`/`comboApi`/`IDS_LBL_API`，条目 `IDS_API_DXGI`「DXGI（自动回退）」/`IDS_API_GDI`「GDI（兼容）」，索引 `kApiSelDxgi=0`/`kApiSelGdi=1`），置于码率行右侧与显示器/帧率/码率风格一致；设置持久化沿用 `PreferGdi` 键（0=DXGI、1=GDI，旧配置自动映射无需迁移）；`WM_COMMAND` 归入 `CBN_SELCHANGE` 即改即存；录制/准备期与其他组合框一同禁用。E2E ALL GREEN；探针实测：下拉 2 项中文文本正确、ini `PreferGdi=1` 启动后选中 GDI 项

* 录制时可以开放部分选项，如输出(如果可以直接改就直接改，不能直接改就最后再移动)和录制鼠标指针等
  * Solution: 鼠标开关**实时生效**——`IScreenCapture::setDrawCursor`，后端 `drawCursor_` 改 `std::atomic<bool>`，`RecorderSession::setDrawCursor` → `runLoop` 每帧应用；输出路径**不可直接改**（MF Sink 绑定 URL）→ `handleFinished` 时若路径已改则 `MoveFileExW` 移动成品（含建目录、失败弹窗回退旧路径）；`setUiRecording` 开放 鼠标复选/路径框/浏览，显示器/帧率/码率/GDI 保持禁用。实测：录制中三者状态正确、切鼠标不崩溃、改路径后文件落到 `Moved-*.mp4` 且无原名残留。**准备期**：控件本就可用（快照前改动无效仅持久化），会话建立时同步鼠标开关；准备超 10ms 显示视觉时同样禁用固定选项（与录制一致）
* 托盘右键菜单加一项退出
  * Solution: `IDS_TRAY_QUIT` + `IDM_TRAY_QUIT`（分隔线下，托盘可见期间可用）：确认框统一为「正在录制，停止并退出？」（`IDS_CONFIRM_QUIT`，Ready 无托盘故无需另一文案），「确定」后 `g.quitting` → 停止保存 → `handleFinished` 销毁窗口；**准备期间**点退出时 `g.sess` 未建立——`onStart` 完成后立即 `requestStop()`，初始化失败路径亦按 quitting 销毁。实测：录制中确定 319ms 完成保存→退出，文件完整；Ready 关窗=直接退出（122ms，无托盘无确认框）。**准备/保存期间**：托盘菜单除「退出」全部置灰（`interactions` 仅 Recording/Paused 为真），左键/双击/中键消息直接忽略（实测准备期发送后状态不变，录制期单机暂停/恢复正常）
* Win32 原生化：`std::thread`→`_beginthreadex`+Event、`std::mutex`→SRWLOCK、`sleep_*`→`Sleep`、`steady_clock`→QPC；移除 `<filesystem>`；函数内 static 移文件作用域；残留 pthread 符号以 `pthreadshim.cpp` 垫片——单文件 exe
* 改用 Win32 原生线程/互斥，消除 `libwinpthread-1.dll` 依赖（单文件 exe）
  * Solution: 逐项替换见上；实测导入表仅剩系统 DLL，独立目录单 exe 录制/暂停/停止/退出（114ms）全绿；CMake 移除 DLL 复制，CI 打包改为单 exe + `RELEASE.md`（精简 README）+ `LICENSE`
* 任务栏角标加一个灰色细边框，避免和图标混淆
  * Solution: `makeDotIcon` 改双圆 4×4 超采样：内圆状态色（`r=size*0.30`）+ 外圈灰环（`RGB(160,160,160)`，厚 `max(1px, size*0.08)`），像素色 = 子样本按 点/环 占比混合、alpha = 总覆盖率；亮红与暗红两个相位角标均带环
* 任务栏角标闪烁有渐显和渐隐，应改为没有(在闪烁时)
  * Solution: 渐变来自"图标↔NULL"的添加/移除动画；改为**图标↔图标**交替——`applyBlinkPhase` 在红点角标与暗红点角标（`overlayBlank`，缓存，`RGB(110,16,16)`；全透明方案在部分任务栏渲染为黑色，弃用）间切换 `SetOverlayIcon`，即时无过渡；离开录制态仍用 NULL 一次清除
* 任务栏角标较大，应改为适当
  * Solution: `makeDotIcon` 圆点直径从占满画布改为画布的 ~60%（`r = size*0.30`），居中抗锯齿绘制
* 删除任务栏图标闪烁，改成角标闪烁
  * Solution: 主图标（大/小）保持稳定不闪；录制中经 `ITaskbarList3::SetOverlayIcon` 显示带灰环的红色圆点角标，`onBlinkTick` 以 1.2s 相位在亮红↔暗红图标间交替（无渐变）；离开录制态 `clearTaskbarOverlay()`
* 将灰色改成蓝色
  * Solution: `stateColor` 的 Preparing/Saving 分支改为 `RGB(0,120,215)`（Windows 强调蓝），图标缓存桶 3 同步
* 在准备、录制、暂停、保存中时，显示托盘
  * Solution: 新增 `src/tray.cpp`：托盘生命周期由 `setStatus`/`handleFinished` 驱动；右键菜单（显示界面/暂停继续/停止，`TrackPopupMenu(TPM_RETURNCMD)` → `IDM_TRAY_*` 走 `WM_COMMAND`）；左键单击=暂停/继续（`TIMER_TRAYCLICK` 按 `GetDoubleClickTime` 防双击误触，双击后尾随 UP 被忽略）；双击=显示；中键=停止；停止后 `handleFinished` 自动 `SW_SHOW`+`SetForegroundWindow`；托盘可见期间 `WM_CLOSE` 隐藏窗口而非退出（Ready 态正常退出）；托盘闪烁=红↔暗红（角标仅任务栏按钮支持，托盘用换图标闪烁）
* 若准备及保存时间<10ms(常量)则不显示该状态
  * Solution: "不显示"=不做任何视觉变更（按钮禁用/圆点/图标/托盘/标题全部延后）。`kTransientStateMs=10`；`onStart` 在后台线程跑阻塞初始化+主线程泵消息，超时才显示准备中视觉；`doStop` 设 `TIMER_SAVING` 10ms，超时且未完成才显示保存中视觉。实测：10ms 窗口内按钮保持可用，超时后正确出现准备中/保存中
* 将默认路径改为CAPTURES\Capture-YYYYMMDD-HHMMSS.mp4
  * Solution: `defaultRecordingTemplate()` 返回字面令牌路径；实测落盘 `OneDrive\Videos\Captures\Capture-20261010-*.mp4`
* 将路径中的VIDEOS渲染为视频文件夹，将CAPTURES渲染为捕获文件夹(若无法查询则默认为VIDEOS\Captures)，将PICTURES渲染为图片文件夹，将SCREENSHOTS渲染为屏幕截图文件夹(若无法查询则默认为PICTURES\Screenshots)
  * Solution: `expandRootToken()` 在 `expandRecordingPath` 内展开首段令牌：`VIDEOS`/`PICTURES` 直接查已知文件夹；`CAPTURES`/`SCREENSHOTS` 内嵌 GUID（MinGW 头缺 `FOLDERID_Captures`，即 `{EDC0FE71-…}`），查询失败回退 `<Videos>\Captures` / `<Pictures>\Screenshots`；令牌路径保留可移植性，展开发生在保存时刻
* 有些渲染细节可能会影响性能，尤其是在低端设备上，建议进行性能优化，或者在低端设备上禁用这些渲染细节
  * 标题栏和任务栏图标
  * 边缘抗锯齿
  * 其他(若有)
  * Solution: 审计结论——图标切换走预生成缓存（零重生成）；圆点/边框均为事件驱动重绘（仅状态变化时），本就无持续开销，故无需低端设备开关。实际高频路径是 `onPoll` 200ms 的标签更新：新增 `setTextIfChanged`（标题+4 个信息列脏检查，文本未变跳过 `WM_SETTEXT` 往返），`clearStatsLabels` 同步重置缓存
* 在录制过程中让大图标的红点闪烁
  * Solution: 图标缓存扩桶（暗红 `kIconBucketRedDim`），`onBlinkTick` 相位翻转任务栏大图标红↔暗红；小图标（标题栏）不闪。（后按需求改为任务栏角标方案，见上）
* 写开源许可证
  * Solution: 采用 GNU General Public License v3.0，`LICENSE` 为 GNU 官方 `gpl-3.0.txt` 全文（553 行），`README.md` 引用
* 将默认按钮(没有使用tab时按enter会点击这个按钮)设为开始停止。
  * Solution: 开始/停止按钮加 `BS_DEFPUSHBUTTON` 样式，配合消息循环中的 `IsDialogMessageW`，未 Tab 时按 Enter 触发开始/停止
* 就绪和准备中不需要显示信息
  * Solution: `Ready`/`Preparing` 状态下统计列（时长/fps/大小）清空，状态列仍显示「就绪/准备中」；事件消息（保存成功/录制出错）保留
* 暂停时显示平均帧率，暂停、保存时和保存成功时需要显示平均这两个字，标题栏无需显示
  * Solution: 新增 `statAvgFps`（与帧数同快照计算，修复了 live 帧数/滞后时长导致平均偏高的问题）；暂停/保存中/保存成功显示「平均 X.X fps」（`IDS_STATS_AVG_FMT`），录制中显示即时 fps；标题栏暂停时同样使用 `statAvgFps`，与信息栏数值完全一致（实测均为 57.1 fps）
* 删除信息栏中的帧数，将时长放到第二位，将状态合并到信息栏上
  * Solution: 单行信息栏四列等宽（状态/时长/fps/大小，各 108px，`SS_ENDELLIPSIS` 截断），删除帧数控件与独立状态行；窗口加宽至 500，按钮上移（y 228→196）、总高收紧至 240
* 在录制边界加一个红色框
* 将保存路径改为Recording-YYYYMMDD-HHMMSS.mp4(两个MM需要区分)保存时替换日期和时间确定实际路径
  * Solution: 路径框显示字面模板；`settingsFromUi` 在开始时经 `expandRecordingPath()` 替换为实际时间。实测生成 `Recording-20261004-205412.mp4`（默认路径后改为 `CAPTURES\Capture-…`，日期展开后升级为逐令牌，见上）
* 在更改配置时立即保存完整配置
  * Solution: `WM_COMMAND` 监听 `CBN_SELCHANGE`（显示器/帧率/码率）、`BN_CLICKED`（鼠标/GDI）、`EN_CHANGE`（路径）即时调用 `saveUiSettings`；`g.loading` 标志防止启动恢复期间程序化设值触发回写覆盖（曾致 `FpsSel` 被默认值 3 覆盖，已修复并实测重启恢复 60fps）
* Optional: 给程序写结构，方便 Agent 读取
  * Solution: 新增 `ARCHITECTURE.md`（模块地图、数据流、GUI 状态机、线程模型、持久化、MinGW 陷阱清单）
* 将开始录制和停止放到一个按钮里，将开始录制改为开始
  * Solution: `IDC_START` 双态按钮（开始/停止），移除独立停止按钮
* 将文本放到string table里，方便翻译
  * Solution: 新增 `src/resource.h`（`IDS_*`）与 `gui.rc` STRINGTABLE（UTF-8 + `#pragma code_page(65001)`）；`gui.cpp` 全部文案改经 `loadStr()` 加载，翻译只需改 `gui.rc`
* 将'|'改为固定布局，而非文本分隔符
  * Solution: 统计行拆为 4 个独立 `STATIC` 控件，`updateStatsLabels()` 分别更新
* 录制时在标题栏上显示录制信息
  * Description: 在标题栏上显示'(录制中/暂停) 屏幕录制 00:00:21 31fps'
  * Solution: `updateTitle()` 用 `IDS_TITLE_REC_FMT` 组合实际时长与实际 fps，200ms 轮询刷新；实测 `(录制中) 屏幕录制 00:00:16 31 fps`
* 未就绪时显示准备中，灰色圆点
  * Solution: 点击开始先置 `Preparing` 状态（灰点+准备中）并强制重绘，再执行阻塞的会话初始化（圆点色后随"灰改蓝"调整；瞬态延后见上）
* 就绪时显示绿色圆点，显示就绪即可，无需显示点击开始录制
  * Solution: `Ready` = 绿点 `RGB(46,160,67)`，状态文本仅「就绪」
* 保存完成后只要显示保存成功即可，不需要显示保存路径
  * Solution: `IDS_STATUS_SAVED`「保存成功」
* 保存时显示'保存中'，灰色圆点
  * Solution: `doStop()` 置 `Saving`（灰点+保存中），worker 收尾后经 `WM_APP_FINISHED` 转「保存成功」；`onPoll` 在 `Saving` 态跳过覆盖（实测已捕捉到「保存中」）
* 圆点位置向右移动一点
  * Solution: 圆点 x 16→24，右侧统计区起点 46→54（后经多轮微调定位至中心对齐）
* 拆分 gui.cpp 为 gui/icons/settings 三文件，组合框索引常量集中定义
  * Solution: 新增 `gui_internal.h`（`App`/`UiState`/共享声明/`kMonitorSelAllMonitors`、`kMonitorSelFirstMonitor`、`kFpsChoices`、`kFpsDefaultSel`、`kBitrateMbps`、`kBitrateAutoSel`）；`icons.cpp`（状态图标+抗锯齿圆点）、`settings.cpp`（`settingsFromUi`/save/load）；`fillCombos` 与 `settingsFromUi` 均改用常量（实测 60fps 选择→录制 60.2fps、即时保存、图标缓存复用均正常）
* 记住输出位置
  * Solution: 设置持久化到 `%APPDATA%\Recording\settings.ini`（输出目录/显示器/帧率/码率/鼠标/GDI），启动时恢复，开始录制或浏览时保存（后升级为任何更改立即保存，见上）
* 图标不需要临时生成，可以先生成好直接切换。默认图标可能无需加载。
  * Solution: 启动时 `ensureStateIcons()` 一次性预生成各色 × 大小图标缓存于 `g.stateIcons`，`updateStateIcons` 仅做指针切换（实测录制结束复用启动句柄=零重生成）；类图标取缓存绿色图标，不再加载 `icon.ico`（exe 资源图标保留供资源管理器使用）；`WM_DESTROY` 统一 `destroyStateIcons()` 防泄漏
* 就绪时仍然显示默认图标
  * Solution: 根因是 `wWinMain` 在 `ShowWindow` 后用静态 `icon.ico` 再次 `WM_SETICON`，覆盖了 `WM_CREATE` 里设好的绿色状态图标；已删除该静态设置块，类图标直接取预生成的绿色状态图标（实测 class icon == `WM_GETICON` big，启动即绿）
* Optional: 如果保存超过一定时长，将开始停止换成取消
  * Solution: 已实现后按用户决定撤销（原因：MF Finalize 不可中断，取消只能"完成后丢弃文件"，意义有限）。已完整还原 recorder/gui 的取消路径与 `IDS_BTN_CANCEL`/`IDS_STATE_CANCELLED` 字符串；保留的停止流程为：停止→（不可打断的）保存→保存成功
* 删除按钮快捷键(若有)。
  * Solution: 排查全部 UI 字符串（STRINGTABLE）均无 `&` 记号，不存在按钮快捷键，无需删除
