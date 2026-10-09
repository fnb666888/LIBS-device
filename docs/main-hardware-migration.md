# 主分支硬件与通信修复移植

基线：main 632f09f。参考：feature/transport-timing-and-preview-cadence 5de5adb 的硬件与通信修复。
模型接入提交 b472cef 保留在原功能分支，本次不将另一套业务服务结构整体合入主分支。

## 保留的架构

Qt UI 与 C++ businessd 分进程，通过命名管道传递 JSON；业务保留 shared / readiness / measurement / app。
ParameterService 管理参数，MeasurementJob 组织连续测量，FrameQueue 分发完整光谱，Writer 异步落盘。
没有引入原分支的 SpectrumStream 后台采集器或 LaserFireService，也没有恢复旧 laser_fire/laser_run 命令。

## 启动和停止

1. 读取参数库及本次请求覆盖，检查门禁和积分时间与激光周期。
2. Laser::prepareContinuous 配置会话参数，明确写 0x22=0，等待放电状态，保持光闸关闭。
3. 光谱仪配置各通道，依次启动从机和主机的 AVS_Measure。
4. 所有通道布防成功后，驱动调用 on_armed 回调。任务重新检查门禁，再调用 openQGate。
5. 任务持续采集，FrameQueue 和 Writer 分别消费完整帧。
6. 停止设置取消标志，收光停机并确认，然后等待采集线程清理 SDK、结束写盘。设备句柄最后关闭。

布防成功以 SDK 的 AVS_Measure 返回值为依据，没有固定 50 ms 延时。它不证明物理通道时间对齐。
连续测量不会因 emission_hold_ms=3000 自动停止；必须使用 measurement_stop 或退出守护进程。
运行中门禁复查保留主分支实现；复查位于采集循环中，实际响应时间受本次采集等待影响。

## 接入规则

- 同一轮采集由独立 acquisition_mutex 保护。measurement_start 预留出光会话；运行时独立 capture 返回 acquisition_busy。
- 激光控制器串行化完整设备操作，防止门禁查询与收光操作交错串口帧。
- SYNC 接法不变：主机 Pin 6 外触发、从机 SYNC，正向清零光谱仪 LASER OUT 参数。
- hardware_synchronized 不再回显请求值；无独立同步证据时报告 false，requested_sync 单独保留。
- SDK 超时之前已完成的完整组保留；expected_groups/received_groups 报告期望及实收数量。不返回未完成组冒充成功。
- 连续测量每次驱动请求采集一组，避免把多发光谱合并成一个显示帧。
- 启动第一组使用本次实际参数。本次请求明确覆盖的光谱字段对整个会话生效；其余字段继续读参数库。
- 串口收齐响应后按 idle_ms 静默间隔结束，不再一律等到 timeout_ms。
- 命名管道采用可取消 overlapped I/O，取消后等待完成，再释放事件和缓冲区。
- UI 取帧在后台线程执行。轮询异常保留此前数据，线程通过正常退出回收，不调用 terminate。
- status 保留主分支的缓存查询设计，不引入设备锁等待。
- 新增 shutdown 请求：先返回 shutdown_requested，再由守护进程主循环停止通信并执行 Runtime::stop。

## 验证

CMake 后端 13 个测试套件、1222 项检查全部通过；Qt 主界面编译通过。
新增替身 SDK 回归覆盖：全部通道布防后才开光闸、准备失败不开光闸、准备期间门禁变化、后续超时保留完整组、停止取消 TTL 等待、停止再启动、本次光谱覆盖持续生效、速率拒绝、线程异常收光。
实际私有命名管道验证：40 个并发请求、半包客户端隔离、状态和参数查询、无设备启动拒绝、Qt PipeClient 订阅与取帧、shutdown 正常退出。
所有验证均使用替身 SDK 或 --no-devices；未对真实激光器执行出光动作。

```powershell
pwsh -File src/build.ps1
pwsh -File ui/build.ps1 -QtPrefix D:\Qt6\6.11.2\mingw_64
```

现场复核时使用原站点接线配置及参数库；默认工作点为 80 A / 200 us / 1 Hz / 210 us Q 延时，光谱 1.05 ms / 1280 ns / 10000 ms。
需要核对首次启动、首发采集、四通道返回、停止重启、调整参数、异常触发中断，以及实际放电次数与完整光谱组数。
代码和离线测试已完成；迁移后的主分支仍需真机复核。
