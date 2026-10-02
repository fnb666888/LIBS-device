#pragma once

// 海关智能设备接入管理边端的报文构造层。
//
// 这里仅负责把已经得到的业务事实整理成 JSON，不负责模型推理，也不自动联网。
// 尤其要把 LIBS 原始光谱和近红外/模型结果分开：当前设备只有
// wavelength_nm + counts，不能伪造 COMPO_*、predBrandResult 或相似度字段。

#include <cstdint>
#include <string>
#include <vector>

#include "libs/business/json/json.hpp"
#include "libs/error.hpp"

namespace business::integration::heike {

struct EnvelopeInput {
    std::string biz_id;                    // 报关单号
    std::string customs_code;             // 实际处置关区代码
    std::string event_code;               // PISYS*/MODEL* 等由物模型确认
    std::string event_source;             // IP/MQ/设备等，由接入方约定
    std::string message_id;               // UUID
    json::Value event_content = json::object();
    bool event_content_as_string = false; // 附件3要求 JSON 字符串；模型示例为 object
    std::string content_type = "application/json";
    std::string notes;
    std::string publisher;
    std::string receiver_channel_name;
    std::string version;
};

// 校验并构造统一外层报文。contentSize 按 eventContent JSON 的 UTF-8 字节数计算。
// 不写 filePath：本层上传的是内嵌 JSON；filePath 与 eventContent 的二选一由另一条
// 文件目录接入链路负责。
libs::Result<json::Value> make_envelope(const EnvelopeInput& input);

struct DeviceIdentity {
    std::string equipment_id;
    std::string equipment_type;
    std::string customs;
    std::string timeseries;                // yyyy-MM-dd HH:mm:ss
};

libs::Result<json::Value> make_heartbeat(const DeviceIdentity& identity);

// code=0 表示设备状态（value 0 正常/1 故障）；code=1 表示开关机
// （value 0 关机/1 开机）。值保留为字符串以匹配附件表格。
libs::Result<json::Value> make_runtime_event(const DeviceIdentity& identity,
                                             const std::string& code,
                                             const std::string& value);

struct DailyStat {
    std::string date;                      // yyyy-MM-dd
    std::int64_t count = 0;
    std::int64_t online_period_s = 0;
};

libs::Result<json::Value> make_openinit(const DeviceIdentity& identity,
                                        const std::vector<DailyStat>& stats);

struct Alarm {
    std::string name;
    std::string type;
    std::string color;
    std::string firsttime;
    std::string time;
    std::string value;
    std::string remake;
};

libs::Result<json::Value> make_alarm(const DeviceIdentity& identity,
                                     const std::vector<Alarm>& alarms);

// 当前 LIBS 采集的可上传内部格式。它不是 PISYS001，也不是 MODEL003：
// PISYS001 需要十项成分结果，MODEL003 需要算法相似度。event_code 由甲方确认后
// 才能填入正式物模型；本函数只构造原始光谱的明确分层载荷。
struct LibsCaptureContext {
    std::string biz_id;
    std::string customs_code;
    std::string belt_code;
    std::string edge_code;
    std::string collect_time;
    std::int64_t seq_no = 0;
    std::string event_source = "LIBS-device";
    std::string message_id;
    std::string event_code;                // 外部确认后的 LIBS 原始谱事件代码
};

libs::Result<json::Value> make_libs_raw_event(const LibsCaptureContext& context,
                                              const json::Value& raw_capture,
                                              bool event_content_as_string = false);

// MODEL001..004 只允许透传调用方已经得到的模型结果（对象或 JSON 对象字符串）；
// 这里不生成任何预测字段。
libs::Result<json::Value> make_model_event(const EnvelopeInput& input);

// 用于构造 URL 路由，不包含具体网络发送。
std::string model_event_name(const std::string& event_code);

}  // namespace business::integration::heike
