# Fixed Bugs
* 录制时显示的帧率总是.0
  * Solution: statFps 原为每秒整数帧数（必为整数）；改为除以真实经过时间（1.00–1.03s 窗口）得带小数 fps；暂停恢复时重置统计窗口。实测 55.1/55.4 fps
* 时长一直是0
* 大小未实时更新
* 准备中时开始按钮没有变灰
    * Solution: onStart 在阻塞初始化前 EnableWindow(btnStart/btnPause, FALSE)，成功后 setUiRecording 恢复为「停止」；另加 state==Ready 防重入守卫
* 大小不正确
    * Description: 录制完成后，显示文件大小与实际文件大小不正确
    * Solution: 实为十进制 MB (÷1,000,000) 与资源管理器 MiB (÷1,048,576) 单位不一致；已统一为 MiB，并在结束时直接重读文件实际大小
* 录制鼠标指针选项无效
    * Description: 关闭录制鼠标指针后，鼠标指针仍然会出现在录制的视频中
    * Solution: GDI 后端的 init 未接收 drawCursor 参数（恒为 true）；已传入并在两条路径（DXGI/GDI）生效。另：输出目录不存在时自动创建（修复 0x80070003）

# Fixed Todos
* 写开源许可证
  * Solution: 采用 GNU General Public License v3.0，LICENSE 为 GNU 官方 gpl-3.0.txt 全文（553 行），README 引用
* 将默认按钮(没有使用tab时按enter会点击这个按钮)设为开始停止。
  * Solution: 开始/停止按钮加 BS_DEFPUSHBUTTON 样式，配合消息循环中的 IsDialogMessageW，未 Tab 时按 Enter 触发开始/停止
* 就绪和准备中不需要显示信息
  * Solution: Ready/Preparing 状态下统计列（时长/fps/大小）清空，状态列仍显示「就绪/准备中」；事件消息（保存成功/录制出错）保留
* 暂停时显示平均帧率，暂停、保存时和保存成功时需要显示平均这两个字，标题栏无需显示
  * Solution: 新增 statAvgFps（与帧数同快照计算，修复了 live帧数/滞后时长 导致平均偏高的问题）；暂停/保存中/保存成功显示「平均 X.X fps」（IDS_STATS_AVG_FMT），录制中显示即时 fps；标题栏暂停时同样使用 statAvgFps，与信息栏数值完全一致（实测均为 57.1 fps）
* 删除信息栏中的帧数，将时长放到第二位，将状态合并到信息栏上
  * Solution: 单行信息栏四列等宽（状态/时长/fps/大小，各 108px，SS_ENDELLIPSIS 截断），删除帧数控件与独立状态行；窗口加宽至 500，按钮上移（y 228→196）、总高收紧至 240
* 在录制边界加一个红色框
* 将保存路径改为Recording-YYYYMMDD-HHMMSS.mp4(两个MM需要区分)保存时替换日期和时间确定实际路径
    * Solution: 路径框显示字面模板 Recording-YYYYMMDD-HHMMSS.mp4；settingsFromUi 在开始时经 expandRecordingPath() 将 YYYYMMDD（月：tm_mon+1）与 HHMMSS（时:分:秒）替换为实际时间；CLI 默认名同步为 Recording-<ts>.mp4。实测生成 Recording-20261004-205412.mp4
* 在更改配置时立即保存完整配置
    * Solution: WM_COMMAND 监听 CBN_SELCHANGE(显示器/帧率/码率)、BN_CLICKED(鼠标/GDI)、EN_CHANGE(路径) 即时调用 saveUiSettings；g.loading 标志防止启动恢复期间程序化设值触发回写覆盖（曾致 FpsSel 被默认值 3 覆盖，已修复并实测重启恢复 60fps）
* Optional: 给程序写结构，方便 Agent 读取
    * Solution: 新增 ARCHITECTURE.md（模块地图、数据流、GUI 状态机、线程模型、持久化、MinGW 陷阱清单）
* 将开始录制和停止放到一个按钮里，将开始录制改为开始
    * Solution: IDC_START 双态按钮（开始/停止），移除独立停止按钮
* 将文本放到string table里，方便翻译
    * Solution: 新增 src/resource.h（IDS_*）与 gui.rc STRINGTABLE（UTF-8 + code_page 65001）；gui.cpp 全部文案改经 loadStr() 加载，翻译只需改 gui.rc
* 将'|'改为固定布局，而非文本分隔符
    * Solution: 统计行拆为 4 个独立 STATIC 控件（时长/帧数/fps/MB），updateStatsLabels() 分别更新
* 录制时在标题栏上显示录制信息
    * Description: 在标题栏上显示'(录制中/暂停) 屏幕录制 00:00:21 31fps'
    * Solution: updateTitle() 用 IDS_TITLE_REC_FMT 组合实际时长与实际 fps，200ms 轮询刷新；实测 '(录制中) 屏幕录制  00:00:16 31 fps'
* 未就绪时显示准备中，灰色圆点
    * Solution: 点击开始先置 Preparing 状态（灰点+准备中）并强制重绘，再执行阻塞的会话初始化
* 就绪时显示绿色圆点，显示就绪即可，无需显示点击开始录制
    * Solution: Ready=绿点 RGB(46,160,67)，状态文本仅"就绪"
* 保存完成后只要显示保存成功即可，不需要显示保存路径
    * Solution: IDS_STATUS_SAVED"保存成功"
* 保存时显示'保存中'，灰色圆点
    * Solution: doStop() 置 Saving（灰点+保存中），worker 收尾后经 WM_APP_FINISHED 转"保存成功"；onPoll 在 Saving 态跳过覆盖（实测已捕捉到"保存中"）
* 圆点位置向右移动一点
    * Solution: 圆点 x 16→24，右侧统计区起点 46→54
* 记住输出位置
    * Solution: 设置持久化到 %APPDATA%\Recording\settings.ini（输出目录/显示器/帧率/码率/鼠标/GDI），启动时恢复，开始录制或浏览时保存
