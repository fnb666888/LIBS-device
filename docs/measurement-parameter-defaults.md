# 测量参数的默认值与优先级

本机默认工作点集中在 `src/include/libs/business/app/measurement_defaults.hpp`，来源为
`docs/real-device-parameters-20260929.md`。只有 `ParameterService` 使用这份默认表。

## 参数来源

1. 启动时先加载 `parameters.json`，再为缺失字段补本机默认值。
2. 已保存的现场参数优先，不会因重启或默认表更新被覆盖。
3. 本次请求中显式指定的 laser / spectrometer 字段优先于参数库，只影响本次会话，不写回参数库。
4. 接线、通道、主从与 SYNC 来自站点配置；激光测量流程始终显式指定外触发。

默认工作点：激光 80 A / 200 μs / 1 Hz / 放电延时 0 μs / Q 延时 210 μs /
Q 预热 0 s；稳定超时 20000 ms / 出光窗口 3000 ms / 最小放电次数 1。
光谱积分时间 1.05 ms / 积分延时 1280 ns / 采集超时 10000 ms。

## 驱动调用

驱动不选择本机工作点。`CaptureRequest` 必须显式传入积分时间、超时、积分延时和触发模式；
`LaserSessionSettings` 必须显式传入六个激光会话参数。省略关键参数无法编译。

```cpp
// m 来自 ParameterService::spectrometer_measurement()。
libs::devices::CaptureRequest request{
    m.integration_ms, m.timeout_ms, m.integration_delay_ns, true
};
// false 表示调用方明确选择自由触发；true 表示等待激光外触发。

// cfg 来自经过业务校验的 FireParameters::laser。
libs::devices::LaserSessionSettings settings{
    cfg.discharge_current_a, cfg.pulse_width_us, cfg.frequency_hz,
    cfg.discharge_delay_us, cfg.q_delay_us, cfg.q_warmup_s
};
```

仓库内调用已同步调整。外部 C++ 调用者需要更新构造方式；JSON IPC 字段保持兼容。
业务数据结构 `SpectrumMeasurement` / `FireParameters` 的积分时间和超时初值为 0，表示未配置；
测量启动校验会拒绝这些值，不再隐式回落到 10 ms / 3000 ms。
设备读回结构的初始零值仍表示尚未读取的事实，不是待下发的工作参数。

## 允许范围

范围统一定义在 `src/include/libs/measurement_limits.hpp`。界面输入、参数保存、请求覆盖、
测量启动与直接驱动调用共用规则。默认表中的每个值也经过编译期范围检查。

| 参数 | 允许范围 | 依据 |
| --- | --- | --- |
| 放电电流 | 15~100 A，整数 | 激光协议 §3.1 |
| 放电脉宽 | 50~300 μs，整数 | 激光协议 §3.1 |
| 放电频率 | 1~10 Hz，整数 | 激光协议 §3.1 |
| 放电延时 | 0~500 μs，整数 | 激光协议 §3.1 |
| Q 延时 | 2~500 μs，整数 | 激光协议 §3.1 |
| Q 预热 | 0~3600 s，整数 | 软件限制；厂家协议未公开量程 |
| 稳定等待超时 | 1~600000 ms，整数 | 软件限制 |
| 出光保持时间 | 0~600000 ms，整数 | 软件限制 |
| 最小放电次数 | 1~100000 次，整数 | 软件限制 |
| 积分时间 | 0.002~60000 ms | SDK 通用下限与项目软件上限 |
| 采集超时 | 1~600000 ms，整数 | 软件限制 |
| 积分延时 | 0~1000000 ns | 软件限制；下发时换算为 FPGA 计数 |

SDK 手册 `AvaSpec Library Manual.txt` §3.4.2 的通用积分上限为 600000 ms，
本项目保留 60000 ms 软件上限。具体探测器的最小积分时间不同，例如 ILX554 为 1.05 ms，
不能把通用 0.002 ms 当成每台仪器的最小值；具体型号由厂家 SDK 的准备测量操作继续校验，
准备失败时不会开启激光光闸。

激光测量还要求“积分时间 + 积分延时 < 激光周期”。自由触发下非零积分延时会被拒绝。
历史参数库中的越界值保留用于诊断，但齐备状态为 false、测量被阻止；修正并保存后恢复。
所有无符号整数转换都在范围检查之后进行，避免负数、大整数或小数转换后变成另一组参数。

## 验证

后端 13 个离线测试套件通过。回归覆盖：省略驱动必填参数无法构造、未配置的业务参数在设备写入前拒绝、
部分参数库补缺失字段、保存参数优先、请求覆盖不写回、重启保留现场值，以及外触发/自由触发的 SDK 配置。
范围回归覆盖每个字段的上下边界、越界、小数整数校验、大整数转换、批量拒绝不部分更新、
直接参数库调用、驱动调用在设备事务前拒绝，以及历史越界参数阻止测量并可修复。
此次未执行真实设备出光。
