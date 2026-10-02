#pragma once

// 海关智能设备接入管理边端的报文构造层。
//
// 这里仅负责把已经得到的业务事实整理成 JSON，不负责模型推理，也不自动联网。
// LIBS 与近红外复用同一反馈字段；来源差异保留在内部，不改变线上报文。
// 原始 wavelength_nm + counts 必须先经算法得到结果，不能伪造成成分或相似度。

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

// LIBS/近红外共用的 PISYS001 成分反馈。eventContent 接收对象或 JSON 对象串，
// 输出固定为附件中的 19 个字段组成的 JSON 字符串。ID 必填，其他缺失值保留
// null；不推断 DATA_SOURCE，不把元素 P 换算或重命名成 P2O5。
// 此接口的 eventSource 可选，缺失时不写入外层报文。
libs::Result<json::Value> make_composition_event(const EnvelopeInput& input);

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

// 原始光谱仅用于本地诊断，不是正式反馈报文。PISYS001 需要成分结果，
// MODEL003 需要算法相似度；两者均不能直接填入 wavelength_nm + counts。
struct LibsCaptureContext {
    std::string biz_id;
    std::string customs_code;
    std::string belt_code;
    std::string edge_code;
    std::string collect_time;
    std::int64_t seq_no = 0;
    std::string event_source = "LIBS-device";
    std::string message_id;
    std::string event_code;                // 本地诊断标识，禁止使用 PISYS/MODEL 命名空间
};

libs::Result<json::Value> make_libs_raw_event(const LibsCaptureContext& context,
                                              const json::Value& raw_capture,
                                              bool event_content_as_string = false);

// MODEL001..004 只允许透传调用方已经得到的模型结果（对象或 JSON 对象字符串）；
// LIBS 与近红外使用同一结果结构。移除内部来源标记，拒绝混入原始谱。
// 这里不生成任何预测字段。
libs::Result<json::Value> make_model_event(const EnvelopeInput& input);

// 用于构造 URL 路由，不包含具体网络发送。
std::string model_event_name(const std::string& event_code);

}  // namespace business::integration::heike
