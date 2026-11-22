#include "libs/devices/su300.hpp"

#include <algorithm>
#include <array>

#include "libs/measurement_limits.hpp"
#include "libs/protocols/modbus_rtu.hpp"

namespace libs::devices {
namespace {

constexpr std::uint8_t kRead = 0x03;
constexpr std::uint8_t kWrite = 0x06;
constexpr std::uint16_t kControl = 0x2000;
constexpr std::uint16_t kCommunicationLossAction = 0x0500;  // F5-00
constexpr std::uint16_t kCommunicationWatchdog = 0x0602;  // F6-02
constexpr std::uint16_t kFieldStopOnCommunicationLoss = 0x1001;
constexpr std::uint16_t kStopStatus = 1u << 1;  // 2101H: stop
constexpr std::uint16_t kRunOrJogStatus = (1u << 0) | (1u << 2) | (1u << 12) | (1u << 13);
constexpr std::uint16_t kActiveDriveFlags = (1u << 0) | (1u << 2) | (1u << 3) | (3u << 8);

std::uint16_t word(const Bytes& data, std::size_t index) {
    const auto offset = 3 + 2 * index;
    return static_cast<std::uint16_t>((data[offset] << 8) | data[offset + 1]);
}

}  // namespace

Result<Bytes> Su300::transaction(std::uint8_t function, std::uint16_t reg, std::uint16_t value) {
    if (!modbus::is_unicast_address(address_))
        return Error::make(ErrorKind::invalid_argument, "su300_address_invalid", "SU300 requires a unicast address");
    const auto request = modbus::make_request(address_, function, reg, value);
    const auto reply = link_.exchange(request, timeout_ms_);
    if (!reply) return reply.error();
    if (reply.value().receive_errors)
        return Error::with_raw(ErrorKind::transport, "su300_uart_error", "SU300 串口接收错误", reply.value().bytes);
    if (reply.value().bytes.empty())
        return Error::make(ErrorKind::timeout, "su300_timeout", "SU300 未返回数据");

    const bool reading = function == kRead;
    const auto frame = modbus::locate_reply(reply.value().bytes, address_, function, request,
        reading ? 5 + 2 * value : request.size(), [&](const std::uint8_t* data) {
            return reading ? data[2] == 2 * value : std::equal(request.begin(), request.begin() + 6, data);
        }, reading);
    if (!frame) return frame.error();
    if (frame.value().exception)
        return Error::with_raw(ErrorKind::device, "su300_exception",
            "SU300 返回 Modbus 异常 " + std::to_string(frame.value().exception_code), frame.value().bytes);
    return frame.value().bytes;
}

Result<Su300Status> Su300::read_status() {
    const auto output = transaction(kRead, 0x2100, 4);
    if (!output) return output.error();
    const auto state = transaction(kRead, 0x2114, 4);
    if (!state) return state.error();

    Su300Status status;
    status.fault_code = word(output.value(), 0);
    status.reference_centihz = word(output.value(), 2);
    status.output_centihz = word(output.value(), 3);
    status.inputs = word(state.value(), 0);
    status.run_word = word(output.value(), 1);
    status.flags_word = word(state.value(), 2);
    status.direction_word = word(state.value(), 3);
    const auto flags = status.flags_word;
    status.communication_control = ((flags >> 12) & 3) == 2;
    status.jogging = (flags & 4) != 0;
    const auto run_status = status.run_word;
    // SU300-Timing-fixed uses 2101H to identify an active jog on the installed drive.
    if ((run_status & 0x3007) == 0x3005) {
        const auto direction = run_status & 0x0018;
        if (direction == 0x0008) status.direction = Su300Direction::forward;
        if (direction == 0x0010) status.direction = Su300Direction::reverse;
        status.jogging = status.direction != Su300Direction::unknown;
        status.jog_direction_from_run_word = status.jogging;
        return status;
    }
    switch (status.direction_word) {
        case 0: status.direction = Su300Direction::forward; break;
        case 1: status.direction = Su300Direction::reverse; break;
        case 2:
            if (!(run_status & kRunOrJogStatus) && !(flags & kActiveDriveFlags) && status.output_centihz == 0)
                status.direction = Su300Direction::stopped;
            break;
        case 3: {
            // The installed drive reports an undocumented 2117H value. Require the
            // independent 2101H stop bit and idle 2116H flags before confirming stop.
            if ((run_status & kStopStatus) && !(run_status & kRunOrJogStatus) &&
                !(flags & kActiveDriveFlags) && status.output_centihz == 0 && status.fault_code == 0) {
                status.direction = Su300Direction::stopped;
            }
            break;
        }
        default: break;
    }
    return status;
}

Result<Su300JogProfile> Su300::read_jog_profile() {
    const auto resolution = transaction(kRead, 0x0018, 1);  // F0-24
    if (!resolution) return resolution.error();
    if (word(resolution.value(), 0) != 0)
        return Error::make(ErrorKind::unsupported, "su300_frequency_resolution", "SU300 未使用 0.1 Hz 频率分辨率");

    const auto jog = transaction(kRead, 0x0109, 4);  // F1-09 through F1-12
    if (!jog) return jog.error();
    const auto loss = transaction(kRead, kCommunicationLossAction, 1);
    if (!loss) return loss.error();
    const auto watchdog = transaction(kRead, kCommunicationWatchdog, 1);
    if (!watchdog) return watchdog.error();

    return Su300JogProfile{word(jog.value(), 0) / 10.0, word(jog.value(), 1) / 10.0,
                           word(jog.value(), 3) * 100u, word(watchdog.value(), 0) * 100u,
                           word(loss.value(), 0)};
}

VoidResult Su300::set_jog_frequency(unsigned tenths_hz) {
    using namespace libs::measurement_limits;
    if (tenths_hz < kMotorJogMinTenthsHz || tenths_hz > kMotorJogMaxTenthsHz)
        return Error::make(ErrorKind::invalid_argument, "su300_jog_frequency_range", "点动频率须在 0.1–50.0 Hz 范围内");
    const auto status = read_status();
    if (!status) return status.error();
    if (!status.value().stopped() || status.value().fault_code)
        return Error::make(ErrorKind::device, "su300_jog_frequency_not_stopped", "设置频率前须确认停机、0 Hz 且无故障");

    const auto resolution = transaction(kRead, 0x0018, 1);  // F0-24
    if (!resolution) return resolution.error();
    if (word(resolution.value(), 0) != 0)
        return Error::make(ErrorKind::unsupported, "su300_frequency_resolution", "SU300 未使用 0.1 Hz 频率分辨率");
    const auto limits = transaction(kRead, 0x0004, 2);  // F0-04 maximum and F0-05 upper frequency
    if (!limits) return limits.error();
    if (tenths_hz > std::min(word(limits.value(), 0), word(limits.value(), 1)))
        return Error::make(ErrorKind::invalid_argument, "su300_jog_frequency_device_limit", "点动频率超过变频器当前最高频率或上限频率");

    constexpr std::uint16_t forward_register = 0x0109;
    const auto current = transaction(kRead, forward_register, 2);
    if (!current) return current.error();
    const std::array<std::uint16_t, 2> original{word(current.value(), 0), word(current.value(), 1)};
    std::array<bool, 2> attempted{};
    Error failure;
    for (std::size_t i = 0; i < original.size(); ++i) {
        if (original[i] == tenths_hz) continue;
        attempted[i] = true;
        const auto written = transaction(kWrite, forward_register + static_cast<std::uint16_t>(i),
                                         static_cast<std::uint16_t>(tenths_hz));
        if (!written) { failure = written.error(); break; }
    }
    if (!failure) {
        const auto readback = transaction(kRead, forward_register, 2);
        if (!readback) failure = readback.error();
        else if (word(readback.value(), 0) != tenths_hz || word(readback.value(), 1) != tenths_hz)
            failure = Error::make(ErrorKind::device, "su300_jog_frequency_mismatch", "正反向点动频率回读不一致");
        if (!failure) return VoidResult();
    }
    for (std::size_t i = 0; i < original.size(); ++i) {
        if (attempted[i]) (void)transaction(kWrite, forward_register + static_cast<std::uint16_t>(i), original[i]);
    }
    const auto restored = transaction(kRead, forward_register, 2);
    if (!restored || word(restored.value(), 0) != original[0] || word(restored.value(), 1) != original[1])
        return Error::make(ErrorKind::device, "su300_jog_frequency_restore_failed", "点动频率写入后未能确认恢复原值，请重新读取变频器");
    return failure;
}

Result<std::uint16_t> Su300::read_stop_watchdog() {
    const auto loss = transaction(kRead, kCommunicationLossAction, 1);
    if (!loss) return loss.error();
    if (word(loss.value(), 0) != kFieldStopOnCommunicationLoss)
        return Error::make(ErrorKind::device, "su300_loss_action_unverified", "变频器通讯丢失停机设置尚未核实");
    const auto watchdog = transaction(kRead, kCommunicationWatchdog, 1);
    if (!watchdog) return watchdog.error();
    return word(watchdog.value(), 0);
}

VoidResult Su300::set_communication_watchdog(std::uint16_t deciseconds) {
    const auto written = transaction(kWrite, kCommunicationWatchdog, deciseconds);
    if (!written) return written.error();
    const auto readback = transaction(kRead, kCommunicationWatchdog, 1);
    if (!readback) return readback.error();
    if (word(readback.value(), 0) != deciseconds)
        return Error::make(ErrorKind::device, "su300_watchdog_mismatch", "变频器通讯超时设定回读不一致");
    return VoidResult();
}

VoidResult Su300::send_control(std::uint16_t value) {
    const auto result = transaction(kWrite, kControl, value);
    return result ? VoidResult() : VoidResult(result.error());
}

VoidResult Su300::jog(Su300Direction direction) {
    if (direction == Su300Direction::forward) return send_control(0x0013);
    if (direction == Su300Direction::reverse) return send_control(0x0023);
    return Error::make(ErrorKind::invalid_argument, "su300_direction_invalid", "请选择点动方向");
}

VoidResult Su300::stop() { return send_control(0x0001); }

}  // namespace libs::devices
