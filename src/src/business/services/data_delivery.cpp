#include "libs/business/services/data_delivery.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace business::data_delivery {

namespace {
// 文件名用一次采集的唯一定位：站号 + 带序号 + 采集时刻。避免用 shot_id 单键
// （shot_id 是 SDK 的进程内计数，重启后重复）。
std::string make_filename(const CaptureReference& capture) {
    std::ostringstream name;
    name << "cap_";
    if (!capture.station_id.empty()) name << capture.station_id << "_";
    name << "belt" << capture.belt_sequence << "_";
    name << "t" << capture.captured_at_ms;
    return name.str();
}
}  // namespace

LocalFileSink::LocalFileSink(std::string directory) : directory_(std::move(directory)) {}

libs::Result<DeliveryStatus> LocalFileSink::deliver(const CaptureReference& capture,
                                                     const std::string& local_blob) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
        DeliveryStatus s;
        s.local_error = "cannot create directory: " + directory_ + " (" + ec.message() + ")";
        return s;   // 注意：返回成功但 local_saved=false + local_error，调用方必须检查字段
    }

    const std::string path = directory_ + "/" + make_filename(capture);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        DeliveryStatus s;
        s.local_error = "cannot open file for write: " + path;
        return s;
    }
    out << local_blob;
    out.flush();
    const bool good = static_cast<bool>(out);
    out.close();

    DeliveryStatus s;
    if (!good) {
        s.local_error = "write failed (possibly disk full): " + path;
        return s;
    }
    s.local_saved = true;
    s.local_path = path;
    // gateway_delivered 保持 false：本地保存这一动作不构成海关交付。
    return s;
}

libs::Result<DeliveryStatus> UnsupportedGatewaySink::deliver(const CaptureReference& capture,
                                                             const std::string& local_blob) {
    (void)capture;
    (void)local_blob;
    DeliveryStatus s;
    s.gateway_error = "gateway delivery unsupported: " + prerequisites();
    // s.gateway_delivered 保持 false。绝不伪造"已交付"。
    return s;
}

std::string UnsupportedGatewaySink::prerequisites() {
    return "① 上传协议与载荷格式 ② 接收地址与端口 ③ 交付确认层级 "
           "④ IPC->网关 与 网关->海关 的确认区分"
           "（任务书 §6.3 未给出网关协议/地址/端口/海关格式；范围报告 §3.6.2 G1/G2）";
}

}  // namespace business::data_delivery
