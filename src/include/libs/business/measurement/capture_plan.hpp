#pragma once

// 业务二：激光触发光谱检测（measurement）—— 采集请求构造（纯函数）。
//
// ── 为什么单独成文件 ────────────────────────────────────────────────────
//
// 采集请求的每个字段都必须有**唯一归属**，否则迟早出现"界面改了 A，
// 实际生效的是 B"。这个函数把归属钉死成两条：
//
//   来源                        | 提供什么
//   --------------------------- | ------------------------------------------
//   装机配置（site）            | 接了哪几台、主从、SYNC
//   测量参数（参数库，函数入参） | 积分时间、超时、积分延时、扫描次数
//
// 曾经 `integration_delay_ns` 在 SpectrometerConfig 里和参数库各存一份 ——
// 界面改了参数库的积分延时，实际生效的是配置里的另一个值。
// 现在它只从测量参数来，site 那一份被彻底删掉。
//
// ── 纪律 3：`wait_for_laser_trigger` 硬编码 true，不可配置 ────────────────
//
// 触发源就是激光器，光谱仪从 pin 6 等它的 TTL（AvaSoft 手册 §4.9 接法二）。
// 这是接法决定的物理事实，不是可调项 —— 配置没填也必须等。
// 旧代码把它做成"可配置"，于是只出光不采集的诊断路径悄悄拍出一堆背景谱，
// 操作员以为那是测量值。
//
// 本函数是**纯函数**：不碰设备、不起线程、不读全局状态，
// 所以"哪个字段归谁"这件事可以被离线测试直接钉住。

#include "libs/business/measurement/frame.hpp"
#include "libs/business/shared/config/site_config.hpp"
#include "libs/devices/spectrometer.hpp"

namespace business::measurement {

// 按上面两条归属构造一次采集请求。**纯函数。**
libs::devices::CaptureRequest build_capture_request(const config::SiteConfig& site,
                                                     const SpectrumMeasurement& measurement);

}  // namespace business::measurement