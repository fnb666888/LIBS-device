#include "libs/business/measurement/writer.hpp"

#include <utility>

#include "libs/business/shared/json/json.hpp"

namespace business::measurement {

Writer::Writer(data_delivery::DeliverySink& sink, std::string station_id)
    : sink_(sink), station_id_(std::move(station_id)) {}

Writer::~Writer() { stop(); }

void Writer::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) return;
    // 清掉上一次 stop() 留下的停止标志。**少了这一行，作业就再也起不来了** ——
    // 而 start() 照样返回成功，现场表现是"点了开始，status 说没在跑，也没报错"。
    stopping_ = false;
    thread_ = std::thread([this]() { loop(); });
}

void Writer::stop() {
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread_.joinable()) return;
        stopping_ = true;
        // 线程把队列里剩下的帧写完才退出 —— 停机不是丢数据的理由。
        thread = std::move(thread_);
    }
    // 通知收尾。join 在**不持锁**的状态下做：线程自己也要拿这把锁。
    cv_.notify_all();
    if (thread.joinable()) thread.join();
}

void Writer::enqueue(const SpectrumFrame& frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kDiskQueueLimit && !queue_.empty()) {
            // 队列满：丢最旧的一帧，如实计数。
            // **绝不反压到采集线程** —— 磁盘慢不能拖住出光节奏（纪律 4）。
            // 这一发光谱因此没存下来，必须让 status 如实暴露，
            // 而不是假装"每发都存了"。
            queue_.pop_front();
            writes_failed_++;
            backlog_.store(queue_.size());
            last_error_code_ = "disk_backlog_dropped";
            last_error_message_ = "落盘队列积压，已丢弃较早的一帧（磁盘太慢）";
        }
        queue_.push_back(frame);
        backlog_.store(queue_.size());
    }
    cv_.notify_one();
}

void Writer::loop() {
    while (true) {
        SpectrumFrame frame;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                // stopping_ 且队列已空 = 收尾完成。
                if (stopping_) return;
                continue;
            }
            frame = std::move(queue_.front());
            queue_.pop_front();
            backlog_.store(queue_.size());
        }
        persist(frame);
    }
}

void Writer::persist(const SpectrumFrame& frame) {
    // 真正写盘。**不在任何设备锁内** —— 磁盘是慢设备，
    // 拿设备锁写盘会让出光停止命令排队等磁盘。
    data_delivery::CaptureReference ref;
    ref.station_id = station_id_;
    ref.shot_id = frame.shot_id;
    ref.captured_at_ms = frame.wall_clock_ms;
    for (const auto& channel : frame.channels) {
        if (channel.state == ChannelState::present) ref.serials.push_back(channel.serial);
    }
    ref.hardware_synchronized = frame.hardware_synchronized;

    // 载荷含完整 wavelength_nm + counts：本地交付存的是**原始光谱**。
    // 只存 shot_id / 通道名一类的元数据等于存了个空壳。
    //
    // 帧里存的是 ChannelFrame，落盘要的是 ScanGroup，因此这里从帧重建
    // 一次 ScanGroup —— 只在落盘线程里做，不在采集路径上。
    std::vector<libs::devices::ScanGroup> groups;
    {
        libs::devices::ScanGroup g;
        g.shot_id = frame.shot_id;
        g.complete = frame.ok();
        g.hardware_synchronized = frame.hardware_synchronized;
        for (const auto& channel : frame.channels) {
            if (channel.state != ChannelState::present) continue;
            libs::devices::Spectrum s;
            s.serial = channel.serial;
            s.pixels = channel.pixels;
            s.device_time_ticks = channel.device_time_ticks;
            s.wavelength_nm = channel.wavelength_nm;
            s.counts = channel.counts;
            g.spectra.push_back(std::move(s));
        }
        if (!g.spectra.empty()) groups.push_back(std::move(g));
    }

    if (groups.empty()) {
        // 这一帧没有可落盘的光谱（全部通道 absent）。如实计数，
        // 不写一个空文件假装存过。
        writes_failed_++;
        return;
    }

    const std::string blob = json::stringify(data_delivery::to_capture_payload(groups));
    const libs::Result<data_delivery::DeliveryStatus> delivered = sink_.deliver(ref, blob);
    if (delivered && delivered.value().local_saved) {
        writes_ok_++;
        return;
    }

    writes_failed_++;
    std::lock_guard<std::mutex> lock(mutex_);
    if (delivered) {
        last_error_code_ = delivered.value().local_error.empty() ? "local_save_failed"
                                                                : delivered.value().local_error;
        last_error_message_ = "落盘失败：" + delivered.value().local_error;
    } else {
        last_error_code_ = delivered.error().code;
        last_error_message_ = "落盘失败：" + delivered.error().message;
    }
}

std::uint64_t Writer::writes_ok() const noexcept { return writes_ok_.load(); }

std::uint64_t Writer::writes_failed() const noexcept { return writes_failed_.load(); }

std::uint64_t Writer::backlog() const noexcept { return backlog_.load(); }

std::string Writer::last_error_code() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_code_;
}

std::string Writer::last_error_message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_message_;
}

}  // namespace business::measurement