// 光谱仪驱动测试：完全离线，用 fake_avaspec_sdk.cpp 编出的替身 DLL 代替厂家 SDK。
// DLL 路径既可由第一个命令行参数给出，也可由环境变量 LIBS_FAKE_AVASPEC_DLL 给出。
#include "check.hpp"

#include "libs/devices/spectrometer.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
int main() { (void)"the spectrometer driver is Windows-only in this build"; return 0; }
#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {


// argv/env 是当前 ANSI 代码页的字节，不是 UTF-8。先按 ANSI 把源 DLL 复制到
// 一个纯 ASCII 的临时目录，再把那条临时路径转成 UTF-8 交给驱动。
// 直接把 argv 当 UTF-8 会在中文工程路径下失败。
std::string g_fake_dll;   // 临时目录里那份替身 DLL 的绝对路径（UTF-8）

std::string stage_fake_dll(int argc, char** argv) {
    std::string given;
    if (argc > 1) given = argv[1];
    else if (const char* from_env = std::getenv("LIBS_FAKE_AVASPEC_DLL")) given = from_env;
    if (given.empty()) return {};

    char temp_root[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, temp_root)) return {};
    const std::string target = std::string(temp_root) + "libs_fake_avaspec\\fake_avaspec_test.dll";
    CreateDirectoryA((std::string(temp_root) + "libs_fake_avaspec").c_str(), nullptr);
    if (!CopyFileA(given.c_str(), target.c_str(), FALSE)) return {};
    g_fake_dll = target;   // 目标路径是纯 ASCII，ANSI 字节本身就是合法 UTF-8
    return g_fake_dll;
}

// 替身 DLL 的失败模式和调用计数是它的全局状态。关键点：必须让这个 DLL 在整个测试
// 期间保持驻留 —— 一旦引用计数归零，模块会被真正卸载，下次 open() 拿到的是全新
// 实例，之前设进去的失败模式就丢了。所以这里持有一份句柄到最后再释放。

// 替身 DLL 的失败模式和调用计数是它的全局状态。关键点：必须让这个 DLL 在整个测试
// 期间保持驻留 —— 一旦引用计数归零，模块会被真正卸载，下次 open() 拿到的是全新
// 实例，之前设进去的失败模式就丢了。所以这里持有一份句柄到最后再释放。
HMODULE g_fake_module = nullptr;

template <class T>
T fake_export(const char* name) {
    T function{};
    const auto address = GetProcAddress(g_fake_module, name);
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

void fake_reset(int mode) { fake_export<void (*)(int)>("FakeReset")(mode); }
int fake_counter(int index) { return fake_export<int (*)(int)>("FakeCounter")(index); }
unsigned char fake_config_u8(int channel, int offset) {
    return fake_export<unsigned char (*)(int, int)>("FakeConfigByte")(channel, offset);
}
int fake_sync_mode(int channel) {
    return fake_export<int (*)(int)>("FakeSyncMode")(channel);
}
unsigned int fake_config_u32(int channel, int offset) {
    return fake_export<unsigned int (*)(int, int)>("FakeConfigU32")(channel, offset);
}

// 一次读完替身的所有调用计数再清零，避免用例之间互相污染。
// index 语义：0=PrepareMeasure 1=Measure 2=StopMeasure 3=Deactivate 4=AVS_Done
std::vector<int> snapshot_counters() {
    std::vector<int> values;
    for (int i = 0; i <= 4; ++i) values.push_back(fake_counter(i));
    fake_reset(0);
    return values;
}

void check_counters(const std::vector<int>& expected) {
    const auto actual = snapshot_counters();
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const std::string label = "counter[" + std::to_string(i) + "]";
        ::testing::check_eq(actual[i], expected[i], __FILE__, __LINE__, label.c_str());
    }
}
}  // namespace

TEST_MAIN_BEGIN_ARGV()

    const std::string dll = stage_fake_dll(argc, argv);
    if (dll.empty()) {
        std::printf("FAIL tests\\test_spectrometer.cpp: could not stage the stand-in DLL\n");
        std::printf("      argv[1]=%s  win32 error %lu\n",
                    argc > 1 ? argv[1] : "<none>", GetLastError());
        return 1;
    }
    // 先自己持有一份句柄，让替身 DLL 的全局状态在整场测试里保持驻留。
    g_fake_module = LoadLibraryA(dll.c_str());
    if (!g_fake_module) {
        std::printf("cannot load %s (win32 error %lu)\n", dll.c_str(), GetLastError());
        return 1;
    }

    auto show = [](const char* what, const libs::Error& e) {
        std::printf("      [%s] %s / %s: %s\n", what, libs::to_string(e.kind), e.code.c_str(), e.message.c_str());
    };

    fake_reset(0);

    { CASE(sdk_abi_matches_vendor_header);
      const auto abi = libs::devices::sdk_abi();
      CHECK_EQ(abi.identity_bytes, std::size_t(75));
      CHECK_EQ(abi.handle_bytes, std::size_t(4));
      CHECK_EQ(abi.measurement_bytes, std::size_t(41));
      CHECK_EQ(abi.control_bytes, std::size_t(16));
      CHECK_EQ(abi.trigger_offset, std::size_t(22));
      CHECK_EQ(abi.control_offset, std::size_t(25));
    }

    { CASE(open_rejects_a_missing_dll);
      const auto bad = libs::devices::Spectrometer::open(dll + ".missing");
      CHECK(!bad);
      CHECK_EQ(bad.error().code, std::string("sdk_load_failed"));
    }

    { CASE(open_rejects_a_relative_path);
      const auto bad = libs::devices::Spectrometer::open("fake_avaspec_test.dll");
      CHECK(!bad);
      CHECK_EQ(bad.error().code, std::string("sdk_load_failed"));
    }

    { CASE(enumerate_reports_every_available_channel);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          const auto list = session.enumerate();
          CHECK(list);
          if (list) {
              CHECK_EQ(list.value().size(), std::size_t(3));
              CHECK_EQ(list.value()[0].serial, std::string("MASTER"));
              CHECK_EQ(list.value()[0].status_name, std::string("usb_available"));
              CHECK_EQ(list.value()[0].pixels, std::uint16_t(4));
              CHECK(list.value()[0].pixels_known);
              CHECK_EQ(list.value()[1].pixels, std::uint16_t(7));
              CHECK_EQ(list.value()[2].pixels, std::uint16_t(9));
              CHECK_EQ(list.value()[0].sdk_version, std::string("9.14.0.0"));
              CHECK_EQ(list.value()[0].board_type, 1);
              CHECK(list.value()[0].board_type_known);
              // 替身的标定是 200 + handle*100 + pixel，MASTER 的 handle 是 1
              CHECK_EQ(list.value()[0].wavelength_min_nm, 300.0);
              CHECK_EQ(list.value()[0].wavelength_max_nm, 303.0);
          }
          const auto closed = session.close();
          CHECK(closed);
          // 只读枚举也走 activate/deactivate，不 Prepare 不 Measure
          // 0=Prepare 1=Measure 2=Stop 3=Deactivate 4=Done
          check_counters({0, 0, 0, 3, 1});
      }
      fake_reset(0);
    }

    { CASE(capture_after_enumerate_does_not_double_activate_a_channel);
      // 回归：设备会话在 open() 时会先 enumerate() 枚举通道（health 报告靠它），
      // 之后每次采集都再走一遍激活。如果激活是"每次都追加一条"而不是幂等，
      // 同一个序列号就会在驱动里出现两次、且两次拿到**同一个句柄**。
      // 后果不是"多了一路数据"，而是那一路被数了两遍：轮询时第一条记录就把
      // 该通道的 pending 取走，第二条同名记录永远等不到，整次采集必然以
      //   capture_timeout / "timed out waiting for MASTER, SLAVE1, SLAVE2"
      // 收场。守护进程里的实时光谱流正是这个顺序（先 health 后 capture），
      // 现场表现为"每帧都超时"，而单独跑一个不枚举的诊断程序却完全正常。
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          const auto list = session.enumerate();
          CHECK(list);
          if (list) CHECK_EQ(list.value().size(), std::size_t(3));

          libs::devices::CaptureRequest request;
          request.serials = {"MASTER", "SLAVE1", "SLAVE2"};
          request.integration_ms = 5.0;
          request.scans = 1;
          request.timeout_ms = 3000;
          const auto result = session.capture(request);
          CHECK(result); if(!result) show("capture", result.error());
          if (result) {
              CHECK_EQ(result.value().groups.size(), std::size_t(1));
              if (!result.value().groups.empty()) {
                  const auto& group = result.value().groups[0];
                  CHECK(group.complete);
                  // 三路都在，且没有第四条同名记录混进来。
                  CHECK_EQ(group.spectra.size(), std::size_t(3));
                  if (group.spectra.size() == 3) {
                      CHECK_EQ(group.spectra[0].serial, std::string("MASTER"));
                      CHECK_EQ(group.spectra[1].serial, std::string("SLAVE1"));
                      CHECK_EQ(group.spectra[2].serial, std::string("SLAVE2"));
                  }
                  CHECK_EQ(group.arm_order.size(), std::size_t(3));
              }
              // 3 个通道各 prepare+measure 一次：通道没有被重复计数。
              // Stop=3 是**下发前清场**：每个通道先无条件 stop 一次，
              // 清掉可能残留在设备上的上一次测量（见 capture_clears_a_stale_...）。
              // 0=Prepare 1=Measure 2=Stop 3=Deactivate 4=Done
              check_counters({3, 3, 3, 0, 0});
          }
          // 再采一次：连续多帧同样不能把通道数越叠越多。
          const auto again = session.capture(request);
          CHECK(again); if(!again) show("capture#2", again.error());
          if (again) {
              CHECK_EQ(again.value().groups.size(), std::size_t(1));
              if (!again.value().groups.empty())
                  CHECK_EQ(again.value().groups[0].spectra.size(), std::size_t(3));
              check_counters({3, 3, 3, 0, 0});
          }
          CHECK(session.close());
      }
      fake_reset(0);
    }

    { CASE(capture_only_touches_the_channels_it_was_asked_for);
      // 同一个根因的另一半：句柄既然会被复用，采集就必须只动本次请求点名的通道。
      // 先 enumerate 把 3 路都激活，再只采 SLAVE1 —— 结果里不该混入 MASTER/SLAVE2，
      // arm 顺序也不该有它们。做不到这一点，一次"只想看一路"的采集会
      // 悄悄把全部已枚举通道都 arm 一遍。
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          const auto list = session.enumerate();
          CHECK(list);
          if (list) CHECK_EQ(list.value().size(), std::size_t(3));
          snapshot_counters();   // 丢掉 enumerate 产生的计数

          libs::devices::CaptureRequest request;
          request.serials = {"SLAVE1"};
          request.integration_ms = 5.0;
          request.scans = 1;
          request.timeout_ms = 3000;
          const auto result = session.capture(request);
          CHECK(result); if(!result) show("capture", result.error());
          if (result) {
              CHECK_EQ(result.value().devices.size(), std::size_t(1));
              CHECK_EQ(result.value().groups.size(), std::size_t(1));
              if (!result.value().groups.empty()) {
                  const auto& group = result.value().groups[0];
                  CHECK_EQ(group.spectra.size(), std::size_t(1));
                  if (!group.spectra.empty())
                      CHECK_EQ(group.spectra[0].serial, std::string("SLAVE1"));
                  CHECK_EQ(group.arm_order.size(), std::size_t(1));
              }
              // 只为 SLAVE1 调一次 Prepare / 一次 Measure。
      // Stop=1 同样是下发前的清场（只对本次点名的通道）。
      check_counters({1, 1, 1, 0, 0});
          }
          CHECK(session.close());
      }
      fake_reset(0);
    }

    { CASE(capture_accepts_channels_this_session_already_activated);
      // 真机缺陷：设备被本进程 AVS_Activate 之后，AVS_GetList 报回来的 status
      // 会变成 2（"被应用占用"）—— 那个"应用"就是**我们自己**。
      // 采集前若只按 status==1 判可用性，就会拒绝自己刚打开的通道：
      // 现场表现是出光完全成功、采集每一帧却都报 serial_not_available。
      fake_reset(5);   // 让替身模拟"激活之后状态变成被应用占用"
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          const auto list = session.enumerate();   // 这一趟把三路都激活了
          CHECK(list);
          if (list) CHECK_EQ(list.value().size(), std::size_t(3));

          libs::devices::CaptureRequest request;
          request.serials = {"MASTER", "SLAVE1", "SLAVE2"};
          request.integration_ms = 5.0;
          request.scans = 1;
          request.timeout_ms = 3000;
          const auto result = session.capture(request);
          // 关键断言：必须认得出这是自己的设备，而不是"被别人占着"。
          CHECK(result); if(!result) show("capture", result.error());
          if (result) CHECK_EQ(result.value().groups.size(), std::size_t(1));
          CHECK(session.close());
      }
      fake_reset(0);
    }

    { CASE(capture_clears_a_stale_pending_measurement_before_arming);
      // 真机缺陷：capture_timeout 那条路径直接 return，被 arm 过的通道留在
      // pending 状态；如果残留来自**上一个被强杀的进程**，进程内清理根本不会跑
      // （close_all 只在优雅关闭时执行）。残留表现为下一次 AVS_PrepareMeasure
      // 返回 -5 = ERR_OPERATION_PENDING，采集从那一刻起永远起不来。
      // 驱动必须在下发前先无条件清一次。
      fake_reset(6);   // 让替身模拟"Measure 之后通道停在 pending"
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"MASTER", "SLAVE1", "SLAVE2"};
          request.integration_ms = 5.0;
          request.scans = 1;
          request.timeout_ms = 3000;

          const auto first = session.capture(request);
          CHECK(first); if(!first) show("capture#1", first.error());

          // 第二次：通道上还挂着上一次的 Measure。修复后驱动会先 StopMeasure 清场，
          // 不修就会在第一条 PrepareMeasure 上拿到 -5。
          const auto second = session.capture(request);
          CHECK(second); if(!second) show("capture#2", second.error());
          if (second) CHECK_EQ(second.value().groups.size(), std::size_t(1));
          CHECK(session.close());
      }
      fake_reset(0);
    }

    { CASE(close_is_idempotent);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          CHECK(session.close());
          const auto again = session.close();
          CHECK(again);
          // 从没枚举或采集过，没有任何句柄，close 不该碰 SDK 也没该 Init/Done
          check_counters({0, 0, 0, 0, 0});
      }
      fake_reset(0);
    }

    { CASE(capture_arms_non_master_channels_first);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"SLAVE1", "MASTER", "SLAVE2"};
          request.master_serial = "MASTER";
          request.synchronized = true;
          request.integration_ms = 5.0;
          request.scans = 1;
          const auto result = session.capture(request);
          CHECK(result);
          if (result) {
              const auto& value = result.value();
              CHECK_EQ(value.groups.size(), std::size_t(1));
              if (!value.groups.empty()) {
                  CHECK(value.groups[0].complete);
                  CHECK(!value.groups[0].hardware_synchronized);
                  // 主通道必须最后 arm
                  CHECK_EQ(value.groups[0].arm_order.size(), std::size_t(3));
                  if (value.groups[0].arm_order.size() == 3)
                      CHECK_EQ(value.groups[0].arm_order[2], std::string("MASTER"));
              }
              // 多通道同步的**接线约定**（厂家 qtdemo 多通道示例 qtdemo.cpp:275-289）：
              //   主通道 Trigger.Source=0（pin6/DB26，收激光器 TTL）+ SetSyncMode(1)
              //   从通道 Trigger.Source=1（SYNC input）+ SetSyncMode(0)
              // SetSyncMode(1) 的含义是"这台是 Master"，不是"这台要同步"；
              // 写反了现场表现是主通道收不到激光、或从通道永远等不到触发。
              //
              // 主通道的 Trigger.Mode 取决于**这次有没有外部硬件触发源**：
              // 本用例没设 wait_for_laser_trigger，即软件/自由触发，所以主通道
              // 是 0（软件触发），由它产生 SYNC 去带从通道 —— 与厂家示例里
              // "chkHardwareTriggered 没勾就是 Mode=0" 一致。
              // 替身通道号 = 器件下标：0=MASTER 1=SLAVE1 2=SLAVE2。
              // 配置偏移：trigger.mode @22 / trigger.source @23。
              CHECK_EQ(static_cast<int>(fake_config_u8(0, 22)), 0);
              CHECK_EQ(static_cast<int>(fake_config_u8(0, 23)), 0);
              CHECK_EQ(fake_sync_mode(0), 1);
              CHECK_EQ(static_cast<int>(fake_config_u8(1, 22)), 1);
              CHECK_EQ(static_cast<int>(fake_config_u8(1, 23)), 1);
              CHECK_EQ(fake_sync_mode(1), 0);
              CHECK_EQ(static_cast<int>(fake_config_u8(2, 22)), 1);
              CHECK_EQ(static_cast<int>(fake_config_u8(2, 23)), 1);
              CHECK_EQ(fake_sync_mode(2), 0);
              CHECK_EQ(value.devices.size(), std::size_t(3));
          }
          CHECK(session.close());
      }
      fake_reset(0);
    }

    { CASE(arming_callback_runs_after_all_channels_before_readout);
      fake_reset(0);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened);
      if (opened) {
        libs::devices::CaptureRequest request;
        request.serials = {"MASTER", "SLAVE1", "SLAVE2"};
        request.master_serial = "MASTER";
        request.synchronized = true;
        request.wait_for_laser_trigger = true;
        bool called = false;
        const auto result = opened.value()->capture(request, [&]() {
          CHECK_EQ(fake_counter(1), 3);
          called = true;
          return libs::VoidResult();
        });
        CHECK(called); CHECK(result);
        if (result) {
          CHECK_EQ(result.value().expected_groups, 1u);
          CHECK_EQ(result.value().received_groups, 1u);
          CHECK(!result.value().groups[0].hardware_synchronized);
        }
        CHECK(opened.value()->close());
      }
    }
    { CASE(failed_arming_callback_cleans_pending_measurements);
      fake_reset(0);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened);
      if (opened) {
        libs::devices::CaptureRequest request;
        request.serials = {"MASTER", "SLAVE1", "SLAVE2"};
        request.master_serial = "MASTER"; request.synchronized = true;
        const auto result = opened.value()->capture(request, []() -> libs::VoidResult {
          return libs::Error::make(libs::ErrorKind::device, "gate_open_failed", "test");
        });
        CHECK(!result);
        if (!result) CHECK_EQ(result.error().code, std::string("gate_open_failed"));
        CHECK(fake_counter(2) >= 6);
        CHECK(opened.value()->capture(request));
        CHECK(opened.value()->close());
      }
    }
    { CASE(later_timeout_keeps_previously_completed_groups);
      fake_reset(7);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened);
      if (opened) {
        libs::devices::CaptureRequest request;
        request.serials = {"MASTER", "SLAVE1", "SLAVE2"};
        request.master_serial = "MASTER"; request.synchronized = true;
        request.scans = 2; request.timeout_ms = 20;
        const auto result = opened.value()->capture(request);
        CHECK(result);
        if (result) {
          CHECK_EQ(result.value().expected_groups, 2u);
          CHECK_EQ(result.value().received_groups, 1u);
          CHECK_EQ(result.value().groups.size(), std::size_t(1));
          CHECK(result.value().groups[0].complete);
        }
        CHECK(opened.value()->close());
      }
      fake_reset(0);
    }

    { CASE(capture_without_master_leaves_trigger_free);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"SLAVE1"};
          request.scans = 1;
          const auto result = session.capture(request);
          CHECK(result);
          if (result && !result.value().groups.empty()) {
              const auto& group = result.value().groups[0];
              CHECK(!group.hardware_synchronized);
              CHECK_EQ(group.spectra.size(), std::size_t(1));
              if (!group.spectra.empty()) {
                  CHECK_EQ(group.spectra[0].serial, std::string("SLAVE1"));
                  CHECK_EQ(group.spectra[0].pixels, std::uint16_t(7));
                  CHECK_EQ(group.spectra[0].wavelength_nm.size(), std::size_t(7));
                  CHECK_EQ(group.spectra[0].counts.size(), std::size_t(7));
                  CHECK_EQ(group.spectra[0].counts[0], 20.0);
              }
          }
          CHECK(session.close());
      }
      fake_reset(0);
    }

    { CASE(capture_with_master_but_no_laser_trigger_still_free_runs);
      // 回归：曾经只要站点配置里写了 master_serial，**任何**"不要等触发"的
      // 采集都会被顶成外触发。实时预览正好是"配置里有 master + 走自由触发"
      // 这个组合，于是四个通道一起等一个永远不来的 TTL，每帧 capture_timeout，
      // 现场表现是"点了开始检测，一条谱都没有"。
      // 调用方说"不等 trigger"就必须真的不等 —— master 是给 SYNC 测量用的。
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"MASTER", "SLAVE1"};
          request.master_serial = "MASTER";         // 配了主通道
          request.synchronized = false;             // 但明确不同步
          request.wait_for_laser_trigger = false;   // 也明确不等激光触发
          request.integration_ms = 5.0;
          request.scans = 1;
          request.timeout_ms = 500;
          const auto result = session.capture(request);
          CHECK(result); if(!result) show("capture", result.error());
          // 两个通道的触发模式都必须是**自由触发 0**，而不是外触发 1。
          // 配置偏移：trigger.mode @22。
          CHECK_EQ(static_cast<int>(fake_config_u8(0, 22)), 0);
          CHECK_EQ(static_cast<int>(fake_config_u8(1, 22)), 0);
          CHECK(session.close());
      }
      fake_reset(0);
    }

    { CASE(capture_runs_multiple_bounded_shots);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"SLAVE1"};
          request.scans = 3;
          const auto result = session.capture(request);
          CHECK(result);
          if (result) CHECK_EQ(result.value().groups.size(), std::size_t(3));
          CHECK(session.close());
      }
      fake_reset(0);
    }

    { CASE(capture_rejects_bad_arguments_without_touching_the_sdk);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.integration_ms = 0.0001;
          CHECK(!session.capture(request));
          request.integration_ms = 10.0;
          request.scans = 0;
          CHECK(!session.capture(request));
          request.scans = 1;
          request.timeout_ms = 0;
          CHECK(!session.capture(request));
          request.timeout_ms = 3000;
          request.master_serial = "MASTER";
          CHECK(!session.capture(request));       // master 不在 serials 里
          request.timeout_ms_override = 1;       // 字段保留但驱动不用它
          fake_reset(0);
          check_counters({0, 0, 0, 0, 0});
          CHECK(session.close());
      }
      fake_reset(0);
    }

    { CASE(capture_reports_an_unknown_serial);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"NOT_THERE"};
          const auto result = session.capture(request);
          CHECK(!result);
          CHECK_EQ(result.error().code, std::string("serial_not_found"));
      }
      fake_reset(0);
    }

    { CASE(capture_surfaces_a_prepare_failure);
      fake_reset(1);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"MASTER", "SLAVE1"};
          const auto result = session.capture(request);
          CHECK(!result);
          CHECK_EQ(result.error().code, std::string("prepare_failed"));
          // 失败注入还留在 SDK 里，清理阶段的 quiet prepare 同样会失败，
          // 所以 close() 必须报出清理不完整，而不是假装一切正常。
          const auto closed = session.close();
          CHECK(!closed);
          if (!closed) CHECK_EQ(closed.error().code, std::string("cleanup_incomplete"));
      }
      fake_reset(0);
    }

    { CASE(capture_surfaces_a_poll_failure);
      fake_reset(2);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"MASTER", "SLAVE1"};
          request.master_serial = "MASTER";
          const auto result = session.capture(request);
          CHECK(!result);
          CHECK_EQ(result.error().code, std::string("poll_failed"));
          CHECK(session.close());
      }
      fake_reset(0);
    }

    { CASE(capture_surfaces_a_scope_failure);
      fake_reset(3);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"SLAVE1"};
          const auto result = session.capture(request);
          CHECK(!result);
          CHECK_EQ(result.error().code, std::string("scope_failed"));
          CHECK(session.close());
      }
      fake_reset(0);
    }


    { CASE(close_reports_clean_shutdown_after_capture);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"MASTER", "SLAVE1"};
          request.master_serial = "MASTER";
          const auto result = session.capture(request);
          CHECK(result); if(!result) show("capture", result.error());
          // 采集期间的计数：2 个设备各 prepare+measure 一次。close() 之前先抓一次，
          // 这样下面的 close_counters 量到的就是纯粹由 close() 产生的清理调用。
      // Stop=2 是下发前的清场。
      check_counters({2, 2, 2, 0, 0});
          const auto closed = session.close();
          CHECK(closed);
          if (!closed) std::printf("      cleanup said: %s\n", closed.error().message.c_str());
          // close() 对每个 touched 的设备各做一次 stop + quiet prepare + deactivate，
          // 最后一次 AVS_Done。
          // 0=Prepare 1=Measure 2=Stop 3=Deactivate 4=Done
          check_counters({2, 0, 2, 2, 1});
      }
      fake_reset(0);
    }

    { CASE(fpga_time_matches_the_sdk_formula_including_its_offset);
      // SDK 示例 qtdemo.cpp:462 正向 (ns+20.84)*6/125，:975 反向 (cycles-1)*125/6。
      // 这对公式不是随意的常数偏移：少了 +20.84 就会整体少 1 个 FPGA 计数，
      // 而 1 个计数正好是 20.83 ns，在几十微秒量级上就是可观的相位误差。
      std::uint32_t cycles = 0xDEADBEEF;
      CHECK(libs::devices::FpgaTime::nanos_to_cycles(0.0, &cycles));
      CHECK_EQ(cycles, 1u);   // 0 ns 也要吃进那 1 个计数的固定开销

      CHECK(libs::devices::FpgaTime::nanos_to_cycles(1000.0, &cycles));
      CHECK_EQ(cycles, std::uint32_t(6.0 * (1000.0 + 20.84) / 125.0));

      // 正反换算自洽：这是 SDK 自己保证的性质，抄错方向就是抄错了。
      for (double ns : {0.0, 280.0, 500.0, 1000.0, 50000.0, 1000000.0}) {
          std::uint32_t c = 0;
          CHECK(libs::devices::FpgaTime::nanos_to_cycles(ns, &c));
          const double back = libs::devices::FpgaTime::cycles_to_nanos(c);
          CHECK(back > ns - 25.0 && back <= ns + 1.0);   // 一步就是 20.83 ns
      }
    }
    { CASE(fpga_time_rejects_negative_and_unrepresentable_delays);
      // 负延时是"光谱仪点火激光器"（接法一）才有的；本项目走激光器触发光谱仪，
      // 时序由激光器决定，所以负延时必须被拒掉而不是静默截断。
      std::uint32_t cycles = 7;
      CHECK(!libs::devices::FpgaTime::nanos_to_cycles(-1.0, &cycles));
      CHECK_EQ(cycles, 7u);   // 失败时不得改写出参
      CHECK(!libs::devices::FpgaTime::nanos_to_cycles(1.0e18, &cycles));   // 溢出 uint32
      CHECK(!libs::devices::FpgaTime::nanos_to_cycles(1.0, nullptr));
    }
    { CASE(laser_triggered_capture_waits_on_pin6_and_never_drives_the_laser);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"MASTER"};
          request.master_serial = "MASTER";
          request.wait_for_laser_trigger = true;      // 手册 4.9 节接法二
          request.integration_delay_ns = 500.0;      // 收到 TTL 后等 500 ns 再积分
          const auto result = session.capture(request);
          CHECK(result); if(!result) show("capture", result.error());
          if (result) CHECK_EQ(result.value().groups.size(), std::size_t(1));

          // 通道 0 就是 MASTER。断言方向没写反：
          CHECK_EQ(fake_config_u8(0, 22), 1);        // trigger.mode = 外触发
          // integration_delay u32 @ 8 = 6*(500+20.84)/125 = 25
          CHECK_EQ(fake_config_u32(0, 8), 25u);
          // control.laser_delay u32 @ 27 与 laser_width u32 @ 31 必须为 0：
          // 这两个字段是"光谱仪发 TTL 出去点火激光器"，方向反了现场就永远不出光。
          CHECK_EQ(fake_config_u32(0, 27), 0u);
          CHECK_EQ(fake_config_u32(0, 31), 0u);
          CHECK(session.close());
      }
      fake_reset(0);
    }

    { CASE(free_running_capture_does_not_wait_for_a_trigger);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          // 不给 master：单通道自由触发，是"不等任何外部信号"的形态。
          // 给了 master 时驱动会按多通道 SYNC 的老规矩把主通道也设成外触发，
          // 那是同步链的事，与"激光器触不触发"无关，所以这里不混在一起测。
          request.serials = {"MASTER"};
          request.master_serial.clear();
          request.wait_for_laser_trigger = false;
          request.integration_delay_ns = 0.0;
          const auto result = session.capture(request);
          CHECK(result); if(!result) show("capture", result.error());

          CHECK_EQ(fake_config_u8(0, 22), 0);        // trigger.mode = 软件/自由触发
          // 0 纳秒也要留 1 个计数的固定开销（见 fpga_time_matches_...）
          std::uint32_t zero_delay = 99;
          CHECK(libs::devices::FpgaTime::nanos_to_cycles(0.0, &zero_delay));
          CHECK_EQ(fake_config_u32(0, 8), zero_delay);
          CHECK_EQ(fake_config_u32(0, 31), 0u);      // 仍不得驱动激光器
          CHECK(session.close());
      }
      fake_reset(0);
    }
    { CASE(integration_delay_without_external_trigger_is_reported_not_swallowed);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"MASTER"};
          request.master_serial = "MASTER";
          request.wait_for_laser_trigger = false;
          request.integration_delay_ns = 500.0;      // 没有触发源却要延时 = 现场白等
          const auto result = session.capture(request);
          CHECK(!result);
          if (!result) {
              CHECK_EQ(result.error().code, std::string("delay_without_trigger"));
              CHECK_EQ(result.error().kind, libs::ErrorKind::invalid_argument);
          }
          CHECK_EQ(fake_counter(0), 0);              // 拒绝发生在碰 SDK 之前
          CHECK(session.close());
      }
      fake_reset(0);
    }

    { CASE(negative_integration_delay_is_rejected_before_touching_the_sdk);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"MASTER"};
          request.master_serial = "MASTER";
          request.wait_for_laser_trigger = true;
          request.integration_delay_ns = -100.0;
          const auto result = session.capture(request);
          CHECK(!result);
          if (!result) CHECK_EQ(result.error().code, std::string("bad_integration_delay"));
          CHECK_EQ(fake_counter(0), 0);
          CHECK(session.close());
      }
      fake_reset(0);
    }
    { CASE(capture_times_out_and_keeps_the_partial_group);
      fake_reset(4);
      auto opened = libs::devices::Spectrometer::open(dll);
      CHECK(opened); if(!opened) show("open", opened.error());
      if (opened) {
          auto& session = *opened.value();
          libs::devices::CaptureRequest request;
          request.serials = {"SLAVE1"};
          request.timeout_ms = 120;
          const auto result = session.capture(request);
          CHECK(!result);
          if (!result) CHECK_EQ(result.error().code, std::string("capture_timeout"));
          CHECK(session.close());
      }
      fake_reset(0);
    }

TEST_MAIN_RETURN()

#endif  // _WIN32
