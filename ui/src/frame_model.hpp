#pragma once

// 帧数据结构（`ui/src/frame_model.hpp`）。
//
// 刻意与业务层的 `SpectrumFrame` 分成两个类型：**UI 不复用业务层的结构体**。
// 复用就得链接 libs_business，而 libs_business 公开链接 libs_devices（C++ 的
// PUBLIC 依赖会传染给下游链接命令行），等于 UI 反向依赖设备层。跨进程边界
// 只走 JSON，字段名对不上就在这一层暴露出来，而不是悄悄共用一个头文件。

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace libs::ui {

// 一路光谱仪通道。**三态**：`present` 有数据，`absent` 本帧没数据。
// absent 时 nm/counts 是空的，reason 有话可说——UI 必须把它画成
// "这一路没接/没数据"，绝不能补零画一条平线冒充"信号是 0"。
struct ChannelData {
    QString serial;
    bool present = false;
    QString reason;
    QVector<double> wavelength_nm;
    QVector<double> counts;

    // 计数最大值，用于 UI 侧的归一化。**归一化是显示口径，
    // 刻意放在 UI 而不是业务层**：口径一改就该只改 UI，不该改库。
    double peak_counts() const;
};

// 一帧。frame.ok=false 是**整次采集失败**，与"某通道 absent"是两回事。
struct FrameData {
    bool ok = false;
    qint64 frame_index = 0;
    qint64 shot_id = 0;
    qint64 wall_clock_ms = 0;
    qint64 captured_monotonic_ms = 0;
    // 同步事实**原样显示**，不从 requested_sync 推断（§3.10.1）。
    bool hardware_synchronized = false;
    bool requested_sync = false;
    double period_ms = 0.0;
    QVector<ChannelData> channels;
    QString capture_error_code;
    QString capture_error_message;

    bool has_any_present_channel() const;
};

// 从 `spectrum_stream latest` 的响应体解析。**只做搬运，不做判定**：
// 通道有没有、整次采集成功与否，全部按守护进程给的原样读回来。
FrameData parse_frame(const QJsonObject& payload);

}  // namespace libs::ui