#pragma once

// 数据交付出口（能力 6）。
//
// 判定依据：范围报告 §3.6
//   3.6.1 原始光谱本地保存 [A]
//   3.6.2 经网关交付海关 [C]
//
// **本文件的核心是"两个正交的交付状态"**（任务书 §6.3）：
// 网关接收与海关接收不能混为同一种交付成功状态。因此数据结构上就是两个独立字段：
//   local_saved  本地工控机已落盘
//   gateway_delivered 已交付海关
// 二者**不是**一个三态枚举：本地存了不等于海关收到了，海关收到了也不代表本地还留着。
//
// 3.6.2 是 C 类（无网关协议、无网络层 G1/G2），因此只有一个 unsupported 实现，
// 绝不返回"已交付"的假象。

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "libs/business/shared/json/json.hpp"
#include "libs/business/shared/time_util.hpp"
#include "libs/devices/spectrometer.hpp"
#include "libs/error.hpp"

namespace business::data_delivery {

// 把一次采集的 ScanGroup 序列化成**可落盘的载荷**。
//
// 它住在交付层，因为"落盘文件长什么样"是交付格式问题，不是采集协调问题。
// 之前它挂在 CaptureCoordinator 上，而那个协调器已经因为只剩字段搬运而被删掉。
//
// 刻意含完整 wavelength_nm + counts：本地交付存的是**原始光谱**，
// 只存 shot_id / 通道名一类的元数据等于存了个空壳。UI 的显示口径
// （归一化、基线扣除）不在这里做，那是前端的事。
//
// **缺失通道不补零、不占位**：它压根不出现在 channels 里。补零画出来是
// "这条谱是平的"，那是谎报。范围报告 §7 纪律 2。
json::Value to_capture_payload(const std::vector<libs::devices::ScanGroup>& groups);

// 一次采集数据的引用。光谱数据本身由能力 10 产出，这里只带引用与元数据，
// 避免把几 MB 的 counts 数组在层间到处复制。
struct CaptureReference {
    std::string station_id;
    std::uint32_t shot_id = 0;
    std::vector<std::string> serials;     // 参与本次采集的光谱通道
    bool hardware_synchronized = false;   // 原样上送 ScanGroup::hardware_synchronized，不推断
    Milliseconds captured_at_ms = 0;
};

// 交付状态。**两个状态位彼此独立**，不允许合成一个枚举。
struct DeliveryStatus {
    // 本地已保存（能力 6.3.1）。落盘成功才为 true。
    bool local_saved = false;
    std::string local_path;               // 落盘后的文件路径，便于复盘
    std::string local_error;              // 落盘失败原因；local_saved=false 时可能有值

    // 网关已交付海关（能力 6.3.2，当前恒为 false，因为是 C 类）。
    // 只有海关**确认收到**才为 true；"已发出"不算。
    bool gateway_delivered = false;
    std::string gateway_error;            // 不支持时写明前置条件
};

// 交付出口。接口与"网关交付"共用：当前只挂本地实现，网关实现是第二个实现。
class DeliverySink {
public:
    virtual ~DeliverySink() = default;

    virtual std::string name() const = 0;

    // 交付一次采集。**只交付引用与元数据**：本地实现自己知道数据在哪儿。
    // 返回的 status 里 local_saved / gateway_delivered 各自独立表达。
    virtual libs::Result<DeliveryStatus> deliver(const CaptureReference& capture,
                                                 const std::string& local_blob) = 0;
};

// ── 3.6.1 本地实现 [A]──────────────────────────────────────────────────────

// 本地落盘。写文件失败时 local_saved=false 且带 local_error；
// **不假装成功，也不把失败吞掉**。
class LocalFileSink final : public DeliverySink {
public:
    explicit LocalFileSink(std::string directory);

    std::string name() const override { return "local_file"; }
    libs::Result<DeliveryStatus> deliver(const CaptureReference& capture,
                                         const std::string& local_blob) override;

    const std::string& directory() const noexcept { return directory_; }

private:
    std::string directory_;
};

// ── 3.6.2 网关实现 [C]───────────────────────────────────────────────────────
//
// 前置条件：① 上传协议与载荷格式 ② 接收地址与端口 ③ 交付确认层级
//           ④ IPC->网关 与 网关->海关 的确认区分（范围报告 §3.6.2）
//
// 返回 unsupported，**绝不返回 gateway_delivered=true**。见范围报告 §4：
// C 类的正确落地形态是带前置条件的 TODO，不是返回 0/false/空的桩。
class UnsupportedGatewaySink final : public DeliverySink {
public:
    std::string name() const override { return "gateway"; }
    libs::Result<DeliveryStatus> deliver(const CaptureReference& capture,
                                         const std::string& local_blob) override;

    static std::string prerequisites();
};

}  // namespace business::data_delivery
