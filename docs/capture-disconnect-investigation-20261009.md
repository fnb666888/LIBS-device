# UI 采集错误排查与异常处理修复

此前约 5 秒真机出光验证确实通过：四台光谱仪返回 4 帧，十项模型预测与原始数据保存均成功，失败为 0。该结果仅证明当时短时链路可运行，不能替代持续采集、重启或偶发通信异常验证。

## 本次故障证据

真机 `data-hardware\run.jsonl` 显示，用户操作的后续会话成功采集并保存 1 帧，随后约 31 秒累计 226919 次采集失败。停止后多次启动均报 `serial_not_found`：SDK 列表缺少第四台 `1706100U4`。关闭阶段该设备的 StopMeasure、PrepareMeasure、SetSyncMode 返回 `-3`；官方 SDK 头文件定义该码为 `ERR_DEVICE_NOT_FOUND`。

本次第一轮独立、不采集不出光的 SDK 查询，`AVS_Init(0)` 返回 3，连续三次 GetList 都只有 `1706097U4`、`1706098U4`、`1706099U4`。用户反馈厂商软件可以正常识别全部四台；后续新进程查询中，`AVS_Init(0)` 已返回 4，连续三次刷新/列举均包含四台，状态为 USB_AVAILABLE。

因此已确认本项目经历了 SDK 设备列表变化，不能据此认定光谱仪损坏，也尚未证明是哪项 USB、驱动、SDK 会话或软件行为触发了异常。后一轮在刷新前 Init 就已返回 4，不能把恢复归因于刷新调用。模型运行不负责 USB 设备识别，本次成功帧也生成了预测文件。

## 已修复并部署的内容

- 驱动将采集过程中 SDK `-3` 统一归为 `spectrometer_disconnected`，保留调用名、序列号和原始返回码。
- 缺少配置通道、设备不可用或明确掉线时，测量作业立即请求收光、停机并读回确认。
- 其他连续三次采集失败也请求停止；孤立失败可重试，成功帧重置连续失败计数。
- 重试间隔至少 100 ms，手动停止可中断等待，避免原来的高速空转。
- 日志保存采集错误详情、连续失败数和停止回读结果；停止回读失败也保留，绝不标记为已确认停止。
- 不自动重新出光或重连后恢复测量。异常结束后，操作者可以点击“停止检测”确认停止并结束当前会话。

16 套 C++ 测试、1667 项检查通过，新增覆盖设备丢失后只失败一次即停止、连续错误的重试上限、间隔和成功恢复计数，以及真实 SDK 适配接口的 `-3` 故障注入。均使用模拟设备，不进行真机出光。

后端更新在真机 `C:\Users\user\libs\main-model-v2-20261009\businessd.exe`，SHA256 为 `6945e1693fc47eb095dd2eb4b53709eba64d8d7f226d4936bae6b375325c7f3a`。原后端保存在同目录 `businessd-before-disconnect-fix.exe`。前端、模型资产、生产 site.json 与测量参数未改变，仍通过桌面“LIBS 新模型预测”启动。

## 证据位置

## 后续正式 UI 会话的停止原因

正式前端在用户交互桌面启动后，用户操作“开始检测”的会话成功采集并保存 28 帧，保存失败为 0。作业启动约 35.6 秒后，第四台光谱仪返回 `AVS_SetSyncMode 1706100U4 failed: -16`；项目随附的官方 SDK 头文件将 `-16` 定义为 `ERR_COMMUNICATION`。随后重试返回 `serial_not_found`，SDK 列表找不到 `1706100U4`，作业自动停止，并记录 `stop_readback_confirmed: true`。本次总采集失败为 2 次，异常处理没有再高速空转。后续三次启动也因同一序列号缺失失败。

该会话没有 `gate_lost_mid_run` 或“运行期门禁关闭”的记录。运行期间确实在帧间按至少 500 ms 的间隔重新检查激光器状态，但这次停止由光谱仪采集错误触发。运行阶段允许正常开启的 Q 开关，不把正常出光状态作为启动阶段的禁止条件。

当前采集流程每帧都会执行 StopMeasure、SetSyncMode、PrepareMeasure 后重新布防；通信错误首先出现在 SetSyncMode。需要进一步对照已验证分支和 SDK 使用流程，排查重复配置、同步触发及 SDK 会话是否影响持续采集。现有证据只能确定停止原因，尚不能确认通信故障的根因，亦不能据此认定设备损坏。

本次完整日志快照保存为 `build/hardware-verification/20261009/disconnect-investigation/latest-visible-ui-run.jsonl`。本轮仅读取日志和源码，没有启动新的出光测试。

## 无 UI 的两分钟测试：首次尝试未能启动

用户随后授权使用同一后端、模型和参数进行 120 秒无 UI 持续测量。首次尝试未启动任何前端，预检查通过，模型加载后 `measurement_start` 返回 `serial_not_found`，缺少第四台 `1706100U4`。采集为 0 帧，未进入持续出光阶段；finally 路径的 `measurement_stop` 返回成功，明确注明此前未运行。因此本次不能作为两分钟持续运行测试的通过或中途故障结果。

Windows PnP 查询仍显示四台 AvaSpec-USB2/3 为 OK；独立 SDK 刷新诊断进程退出码为 `-1073740940`，没有生成新枚举报告，旧报告不能作为本次设备识别证据。后续识别检查发现用户交互桌面正在运行厂商软件 `D:\AvaSoft7USB2\avaspec78USB2.exe`，因此暂停继续访问设备，等待该程序确认四台并关闭。项目参数、生产配置和后端未修改。

测试脚本为 `verify-headless-2min.ps1`，使用独立 135 秒停机保护、运行状态轮询及故障后不重新出光的策略。首次尝试证据包括同目录下的 `headless-2min-parameters.json`、`headless-2min-precheck.json`、`headless-2min-start.json`、`headless-2min-finally-stop.json` 和 `headless-2min-run.jsonl`。

## 无 UI 两分钟测试：后续完整运行通过

厂商软件确认四台并关闭后，再次启动同一后端和模型，无任何 UI 进程参与；生产参数仍为 80 A / 200 μs / 1 Hz / Q 延时 210 μs，积分时间 1.05 ms / 积分延时 1280 ns / 采集超时 10000 ms。未改生产代码、配置、模型或运行期门禁检查。

从 measurement_start 成功返回后计时 120210 ms，随后主动 measurement_stop。成功采集 119 帧，采集失败 0；保存 119 帧，保存失败 0、积压 0；v2 模型完成 119 次预测，失败 0、不适用 0。停止返回“光闸关、已停机，读回确认通过；落盘线程已收尾”。设备关闭成功，后端退出码 0。SDK 通信错误及运行期门禁关闭均未复现。

本次证明同一后端在状态检查开启的条件下能完成两分钟无 UI 采集与预测，不证明 UI 是此前通信故障的根因；厂商软件访问、关闭和新建 SDK 会话也改变了试验前状态。最新预测仍保留原有 review_required 质量标记，包括同步未独立确认、模型边界适配及超出已观察训练域，运行成功不等于预测精度已验证。

后续完整测试证据为同目录下 `headless-2min-summary.json`、`headless-2min-trace.json`、`headless-2min-measurement.json`、`headless-2min-prediction.json`、`headless-2min-stop.json`、`headless-2min-exit.json` 和 `headless-2min-run.jsonl`。首次尝试的本机证据已复制保存在子目录 `headless-2min-attempt1`，不会与后续成功运行混淆。分支对比见 [持续采集分支对比](continuous-capture-branch-comparison-20261009.md)。

## 无 UI 测试后再次使用 UI 的故障

用户再次通过正式 UI 运行后，成功采集并保存 22 帧、失败 2 次。作业启动约 29.68 秒后 `AVS_SetSyncMode 1706097U4 failed: -16`，后续同一 SDK 会话重试找不到 `1706097U4`，触发自动收光并回读成功。本次失联的是主通道，不是此前的第四台 1706100U4；不能再将排查限定为第四台硬件。

用户停止并关闭 UI 后，launch-ui.ps1 自动发 shutdown，SDK 关闭过程中主通道返回 -3。调查连接时未发现 UI/后端进程，原会话已经结束；本轮未启动后端、未调用 SDK Init/Done/UpdateUSBDevices。现有同会话重试及关闭报错已经证明，当时主通道没有在后端会话内自行恢复，不能把前一次新会话两分钟通过视为故障已解决。

当前 Windows PnP 仍显示四台光谱仪为 OK。检查 UI 自动轮询源码：status 查询缓存（含预测状态）、param_get 查询参数库、measurement_status 查询计数、spectrum_stream latest 读取内存帧队列；这些轮询不直接枚举、配置或采集光谱仪。UI 开始按钮只提交一次 measurement_start。故障与 UI 运行场景存在重复关联，但具体因果仍未确认。

还发现此前无 UI 测试在 SSH 的 Session 0，正式 UI/后端在交互桌面的 Session 1，二者除 UI 外还有运行会话差异。下一轮应控制相同 Session，并在故障后保持后端存活，查看同一 SDK 会话的设备列表；关闭 UI 或通过常规启动脚本重启都会失去这一状态。此次原始证据快照为 `ui-repeat-fault-run.jsonl`。

## 稳定参数下复用 SDK 准备状态的候选修复

桌面 UI 测试首次在出光前就缺少主通道 1706097U4，0 帧。保留后端后，同一 SDK 会话的 capture 仍返回 serial_not_found；并未重新初始化 SDK。这次不是完成两分钟后中断，而是既有失联状态尚未恢复。证据见 `desktop-ui-2min-start-failed-run.jsonl` 及 `desktop-ui-2min-same-session-capture-check.json`。故障证据归档后正常退出，用户再用厂商软件确认四台均可读取并关闭。

候选修复在 Spectrometer 内按通道缓存已成功下发的 41 字节测量配置和 SYNC 值：前一次采集完整成功且参数未变时，下一帧只重新 AVS_Measure 布防、等待触发并读取数据，不重复 StopMeasure、SetSyncMode 和 PrepareMeasure。初次采集、参数/触发配置改变时继续停残留测量并重新准备；任何错误、取消或部分组返回都会使准备缓存失效，下一次重新准备。缓存仅存在于当前 SDK 会话，不跨关闭/重新打开保存；掉线保护、运行期门禁及四通道完整性检查保留。

这减少持续触发期间的重复 USB 控制传输，是根据故障首先发生在 SetSyncMode 所提出的修复方向；不能在真机验证前认定重复配置就是故障根因。模拟 SDK 回归验证连续 120 帧只准备/设置同步各一次，每帧所有通道仍重新布防，并验证参数变化、错误、取消后的重新准备。另修正测试替身从 41 字节 SDK 配置指针读取 64 字节的越界读取，测试检查缓冲区仍保留 64 字节但只复制合法字段。16 套测试、1927 项检查通过。

候选后端 SHA256 为 `247aaccf4e2471659e73434ade19cc93604b2ca15b4a35252f00115678728c0c`，原后端备份为 `businessd-before-stable-preparation.exe`。测试通过 one-off 交互任务在 Session 1 启动当前生产 MainWindow/按钮处理代码构成的专用测试入口，自动点击预检、选择模型、开始与停止；正常前端 exe 和模型未改。UI 成功后计时 120 秒，故障后不自动重新出光，并保留后端 SDK 会话供诊断。

## 修复后的桌面 UI 两分钟验证通过

修复后实际桌面 UI 与后端均为 Session 1，后端 PID 9636、UI PID 7552。测试入口先因状态刷新覆盖预检提示而未继续，尚未出光；修正自动化的完成判断后，仅重开测试 UI，继续连接 PID 9636 的同一 SDK 会话。未重新初始化后端或 SDK。

UI 记录 ui_start_succeeded 后 120107 ms 自动点击停止检测，576 ms 后记录 ui_stop_completed。成功采集、保存和预测各 119 帧；采集、保存和预测失败均为 0，不适用预测 0，保存积压 0。停止回读成功，未复现 SDK -16、通道缺失或运行期门禁关闭。测试截图、控制事件、原始运行日志和各项 JSON 状态归档至 `build/hardware-verification/20261009/disconnect-investigation/desktop-ui-stable-preparation`。本次验证为一次完整两分钟测试，更长时间和多次运行的稳定性仍需观察，尚不能仅据此确定底层通信故障的唯一根因。

测试结束后 UI/SDK 一度保留。随后已为当前测试窗口安装退出清理：等待 UI 关闭，再向同一个后端发送 measurement_stop、确认回读、发送 shutdown 并等待正常退出，不强杀后端。就绪证据为 desktop-ui-2min-cleanup-ready.json。正常生产快捷方式 launch-ui.ps1 原本已有同样的退出顺序。

本机 `D:\LIBS\build\hardware-verification\20261009\disconnect-investigation` 保留本次 `run.jsonl`、错误日志、SDK 查询结果、修复构建日志和不出光诊断脚本。原始短时验证证据见 [模型 v2 部署记录](model-v2-deployment-20261009.md)。
