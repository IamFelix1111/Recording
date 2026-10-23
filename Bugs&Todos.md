# Bugs
* [x] 托盘右键没有显示菜单
  * Solution: 根因——主窗口隐藏到托盘时 `SetForegroundWindow(g.wnd)` 失败，`TrackPopupMenu` 因宿主线程不持有前台而立即关闭（菜单"闪没"）。修复：新增离屏但 `WS_VISIBLE` 的 1×1 宿主窗口 `g_menuHost`（文件作用域+空指针惰性初始化，**禁用函数内 static**——其 guard 会拉入 libstdc++ 的 `pthread_cond_*` 使 `libwinpthread-1.dll` 依赖回归），菜单以宿主为 owner 并在关闭后 `PostMessage(WM_NULL)` 释放捕获。实测隐藏态右键：菜单出现、5 项、存活>1.2s
* [x] 托盘图标不会显示
  * Solution: 根因——某轮瞬态重构把 `onStart` 里的 `trayShow()` 调用删除后未恢复，`setStatus` 只调 `traySetState`（隐藏态空操作），导致托盘永不显示。修复：`setStatus` 进入活动状态（非 Ready）且 `trayIsVisible()` 为假时自动 `trayShow()`（覆盖 准备/录制/暂停/保存 全部入口，含快于 10ms 的初始化路径）；`trayShow` 内 `Shell_NotifyIconW(NIM_ADD)` 失败时保持隐藏态。调试器实测：断点捕获调用，参数校验通过（hWnd/hIcon/uID/回调消息），API 返回 `true`
* [x] 开始录制后会隐藏任务栏(窗口盖住任务栏)
  * Solution: 三轮迭代——①根因：边框单窗口整屏边界+置顶，而 Win11 任务栏（`Shell_TrayWnd`）**不带 `WS_EX_TOPMOST`**，shell"整屏窗口盖住任务栏"即判全屏并自动隐藏任务栏；②v1（插任务栏之下）回归：`insert-after` 非置顶参照把边框拉出置顶带，普通窗口可覆盖（时隐时现）；③v2（底边上收 1px）破全屏判定但留 1px 缝隙；**最终 v3：改为四条独立边条窗口**（`g_borderWnds[4]`=上/下/左/右，各条各自置顶、`WDA_EXCLUDEFROMCAPTURE`、整条填充绘制）——无任何窗口整屏覆盖（不触发全屏判定）、几何像素级完整（底边精确到屏幕底）、任务栏正常。实测：4 条 rect 正确、全 TOPMOST、任务栏 824-864 可见、录制/收尾正常
* [x] See Fixed Bugs in [Fixed Bugs&Todos.md](Fixed%20Bugs%26Todos.md#fixed-bugs)

# Todos
* Every Day Check: 检查并优化代码，若发现有不合理或可改进的地方，及进行调整和优化，大调整需要写入todos。
  * Rule: 每天一次即可；`Last Done` 24 小时内不再重复执行
  * Last Done: 2026-10-10 17:30
  * Notes: See Every Day Check Notes in [Fixed Bugs&Todos.md](Fixed%20Bugs%26Todos.md#every-day-check-notes)
* [ ] 暂不实现: 实现分辨率变化应对方案 v5（设计已定稿冻结并迁移至 [DESIGNS.md](DESIGNS.md#分辨率变化应对方案v5经四轮评审修订已冻结)；实现顺序：recorder 侧分段/临时/拼接基建 → 测试模式回归 → GUI 告知框）
* [x] See Fixed Todos in [Fixed Bugs&Todos.md](Fixed%20Bugs%26Todos.md#fixed-todos)
