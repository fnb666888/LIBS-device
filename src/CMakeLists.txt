cmake_minimum_required(VERSION 3.20)
project(libs_core LANGUAGES CXX)

# ── 目标平台 ──────────────────────────────────────────────────────────────────
# 三层库的移植层用的是 Win32 COM 口和 AvaSpec 的 __stdcall 导出，
# 所以这个库目前只声明支持 Windows x64。
if (NOT WIN32)
  message(FATAL_ERROR "libs_core currently targets Windows x64 only")
endif()
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "libs_core requires a 64-bit toolchain; the AvaSpec handle ABI is 32-bit on x64")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(libs_warnings INTERFACE)
if (MSVC)
  target_compile_options(libs_warnings INTERFACE /W4 /permissive-)
else()
  target_compile_options(libs_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ── 第 3 层：纯协议 ───────────────────────────────────────────────────────────
# 只有字节和值语义，不碰串口、不碰 SDK、不含任何设备或安全策略。
add_library(libs_protocols STATIC
  src/error.cpp
  src/hex.cpp
  src/protocols/modbus_rtu.cpp
  src/protocols/laser.cpp
  src/protocols/rangefinder.cpp
  src/protocols/indicator.cpp
  src/protocols/air_conditioner.cpp
  src/protocols/su300.cpp
)
target_include_directories(libs_protocols PUBLIC include)
target_link_libraries(libs_protocols PRIVATE libs_warnings)

# ── 第 2 层：传输 ─────────────────────────────────────────────────────────────
# 把字节送出去并收回来，带超时；不理解任何协议内容。
add_library(libs_transport STATIC
  src/transport/serial.cpp
  src/transport/serial_timing.cpp
)
target_link_libraries(libs_transport PUBLIC libs_protocols)
target_link_libraries(libs_transport PRIVATE libs_warnings)

# ── 第 1 层：设备驱动 / SDK ──────────────────────────────────────────────────
# 把传输或厂商 SDK 的原始结果翻译成结构化数据。
# 依赖方向严格是 devices -> transport -> protocols，反向没有任何引用。
add_library(libs_devices STATIC
  src/devices/laser.cpp
  src/devices/rangefinder.cpp
  src/devices/indicator.cpp
  src/devices/spectrometer.cpp
  src/devices/air_conditioner.cpp
  src/devices/su300.cpp
)
target_link_libraries(libs_devices PUBLIC libs_transport)
target_link_libraries(libs_devices PRIVATE libs_warnings)

# 上层业务只应该依赖这一个聚合目标。
add_library(libs INTERFACE)
target_link_libraries(libs INTERFACE libs_devices)

# ── 测试 ──────────────────────────────────────────────────────────────────────
include(CTest)
enable_testing()

add_library(libs_test_support STATIC tests/check.cpp)
target_include_directories(libs_test_support PUBLIC tests)
target_link_libraries(libs_test_support PUBLIC libs_protocols)

add_executable(test_protocols tests/test_protocols.cpp)
target_link_libraries(test_protocols PRIVATE libs_test_support libs_warnings)
add_test(NAME protocols COMMAND test_protocols)

add_executable(test_devices tests/test_devices.cpp)
target_link_libraries(test_devices PRIVATE libs_test_support libs_transport libs_devices libs_warnings)
add_test(NAME devices COMMAND test_devices)

# 空调：纯协议层（寄存器映射与功能码编解码）与设备层（收发、超范围拒发）
add_executable(test_air_conditioner_protocol tests/test_air_conditioner_protocol.cpp)
target_link_libraries(test_air_conditioner_protocol PRIVATE libs_test_support libs_protocols libs_warnings)
add_test(NAME air_conditioner_protocol COMMAND test_air_conditioner_protocol)

add_executable(test_air_conditioner_device tests/test_air_conditioner_device.cpp)
target_link_libraries(test_air_conditioner_device PRIVATE libs_test_support libs_transport libs_devices libs_warnings)
add_test(NAME air_conditioner_device COMMAND test_air_conditioner_device)

# SU300 矢量变频器：Modbus-RTU 寄存器/命令编解码与同步设备事务。
add_executable(test_su300 tests/test_su300.cpp)
target_link_libraries(test_su300 PRIVATE libs_test_support libs_transport libs_devices libs_warnings)
add_test(NAME su300 COMMAND test_su300)

# 串口读循环判定：纯逻辑，不打开任何 COM 口。
add_executable(test_serial_timing tests/test_serial_timing.cpp)
target_link_libraries(test_serial_timing PRIVATE libs_test_support libs_transport libs_warnings)
add_test(NAME serial_timing COMMAND test_serial_timing)

# 光谱仪测试需要一个替身 DLL。驱动自己按绝对 UTF-8 路径加载它，
# 而 Windows 的 argv 是 ANSI 字节，中文工程目录下会直接被拒；
# 所以这里把替身 DLL 输出到纯 ASCII 的构建目录，命令行也用纯 ASCII 前缀。
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_library(fake_avaspec SHARED tests/fake_avaspec_sdk.cpp)
  set_target_properties(fake_avaspec PROPERTIES PREFIX "" OUTPUT_NAME "fake_avaspec_test")

  add_executable(test_spectrometer tests/test_spectrometer.cpp)
  target_link_libraries(test_spectrometer PRIVATE libs_test_support libs_devices libs_warnings)
  add_dependencies(test_spectrometer fake_avaspec)
  add_test(NAME spectrometer
    COMMAND test_spectrometer $<TARGET_FILE:fake_avaspec>)
  set_tests_properties(spectrometer PROPERTIES
    ENVIRONMENT "LIBS_FAKE_AVASPEC_DLL=$<TARGET_FILE:fake_avaspec>")
else()
  message(STATUS "test_spectrometer is only wired up for the MinGW toolchain")
endif()



# ── 第 4 层：业务层 ───────────────────────────────────────────────────────────
# 只在 devices 之上组装业务语义，不反向改驱动，也不直接调 SDK。
# 与 build.ps1 里的 libs_business 源文件列表保持一致。
add_library(libs_business STATIC
  src/business/time_util.cpp
  src/business/json/json.cpp
  src/business/config/site_config.cpp
  src/business/integration/heike_events.cpp
  src/business/session/device_session.cpp
  src/business/policy/detection_config.cpp
  src/business/policy/gate.cpp
  src/business/logging/run_log.cpp
  src/business/services/ac_interlock.cpp
  src/business/services/algorithm_contract.cpp
  src/business/services/business_event.cpp
  src/business/services/capture.cpp
  src/business/services/composition.cpp
  src/business/services/data_delivery.cpp
  src/business/services/device_debug.cpp
  src/business/services/environment.cpp
  src/business/services/laser_fire.cpp
  src/business/services/facade.cpp
  src/business/services/motion.cpp
  src/business/services/ore_presence.cpp
  src/business/services/spectrum_stream.cpp
  src/business/app/runtime.cpp
)
target_link_libraries(libs_business PUBLIC libs_devices)
target_link_libraries(libs_business PRIVATE libs_warnings)

# 守护进程：main.cpp 是可执行入口，named_pipe_server.cpp 归守护进程所有，
# 只被 businessd.exe 和离线 IPC 测试复用，所以都不进静态库。
# advapi32 是命名管道 ACL/SDDL 用的。
add_executable(businessd
  src/businessd/named_pipe_server.cpp
  src/businessd/main.cpp
)
target_link_libraries(businessd PRIVATE libs_business advapi32 libs_warnings)

# 业务层测试
add_executable(test_business_core tests/test_business_core.cpp)
target_link_libraries(test_business_core PRIVATE libs_test_support libs_business libs_warnings)
add_test(NAME business_core COMMAND test_business_core)

add_executable(test_business_services tests/test_business_services.cpp)
target_link_libraries(test_business_services PRIVATE libs_test_support libs_business libs_warnings)
add_test(NAME business_services COMMAND test_business_services)

add_executable(test_business_facade tests/test_business_facade.cpp)
target_link_libraries(test_business_facade PRIVATE libs_test_support libs_business libs_warnings)
add_test(NAME business_facade COMMAND test_business_facade)

# IPC 测试既覆盖 handle_line() 协议面，也在测试进程内启动真实命名管道；
# 仍不拖 main.cpp（测试自己控制 NamedPipeServer 的生命周期）。
add_executable(test_business_ipc tests/test_business_ipc.cpp src/businessd/named_pipe_server.cpp)
target_link_libraries(test_business_ipc PRIVATE libs_test_support libs_business advapi32 libs_warnings)
add_test(NAME business_ipc COMMAND test_business_ipc)
set_tests_properties(business_ipc PROPERTIES TIMEOUT 30)

# 出光流程测试：门禁拦截、协议极性、指令序列、失败收光。
add_executable(test_laser_fire tests/test_laser_fire.cpp)
target_link_libraries(test_laser_fire PRIVATE libs_test_support libs_business libs_warnings)
add_test(NAME laser_fire COMMAND test_laser_fire)

# 实时光谱流：租约互斥、通道三态、背压、订阅生命周期。全部离线。
add_executable(test_spectrum_stream tests/test_spectrum_stream.cpp)
target_link_libraries(test_spectrum_stream PRIVATE libs_test_support libs_business libs_warnings)
add_test(NAME spectrum_stream COMMAND test_spectrum_stream)

add_executable(test_algorithm_contract tests/test_algorithm_contract.cpp)
target_link_libraries(test_algorithm_contract PRIVATE libs_test_support libs_business libs_warnings)
add_test(NAME algorithm_contract COMMAND test_algorithm_contract)

add_executable(test_device_debug tests/test_device_debug.cpp)
target_link_libraries(test_device_debug PRIVATE libs_test_support libs_business libs_warnings)
add_test(NAME device_debug COMMAND test_device_debug)
