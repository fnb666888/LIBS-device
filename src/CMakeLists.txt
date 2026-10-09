cmake_minimum_required(VERSION 3.20)
project(libs_core LANGUAGES CXX)

# ── 目标平台 ──────────────────────────────────────────────────────────────────
# 各层库的移植层用的是 Win32 COM 口和 AvaSpec 的 __stdcall 导出，
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

# ── 第 2 层：传输 ─────────────────────────────────────────────────────────────
# 把字节送出去并收回来，带超时；不理解任何协议内容。
add_library(libs_transport STATIC
  src/error.cpp
  src/hex.cpp
  src/transport/serial.cpp
  src/transport/serial_timing.cpp
)
target_include_directories(libs_transport PUBLIC include)
target_link_libraries(libs_transport PRIVATE libs_warnings)

# ── 第 1 层：设备驱动 / SDK ──────────────────────────────────────────────────
# 设备协议是控制器内部实现，不再作为独立公开目标。
add_library(libs_devices STATIC
  src/protocols/modbus_rtu.cpp
  src/protocols/laser.cpp
  src/protocols/rangefinder.cpp
  src/protocols/indicator.cpp
  src/protocols/air_conditioner.cpp
  src/devices/laser.cpp
  src/devices/rangefinder.cpp
  src/devices/indicator.cpp
  src/devices/spectrometer.cpp
  src/devices/air_conditioner.cpp
)
target_link_libraries(libs_devices PUBLIC libs_transport)
target_link_libraries(libs_devices PRIVATE libs_warnings)

# 上层业务只应该依赖设备控制器聚合目标。
add_library(libs INTERFACE)
target_link_libraries(libs INTERFACE libs_devices)

# ── 测试 ──────────────────────────────────────────────────────────────────────
include(CTest)
enable_testing()

add_library(libs_test_support STATIC tests/check.cpp)
target_include_directories(libs_test_support PUBLIC tests)
target_link_libraries(libs_test_support PUBLIC libs_devices)

add_executable(test_serial_timing tests/test_serial_timing.cpp)
target_link_libraries(test_serial_timing PRIVATE libs_test_support libs_transport libs_warnings)
add_test(NAME serial_timing COMMAND test_serial_timing)

add_executable(test_protocols tests/test_protocols.cpp)
target_link_libraries(test_protocols PRIVATE libs_test_support libs_devices libs_warnings)
add_test(NAME protocols COMMAND test_protocols)

add_executable(test_devices tests/test_devices.cpp)
target_link_libraries(test_devices PRIVATE libs_test_support libs_transport libs_devices libs_warnings)
add_test(NAME devices COMMAND test_devices)

# 空调：纯协议层（寄存器映射与功能码编解码）与设备层（收发、超范围拒发）
add_executable(test_air_conditioner_protocol tests/test_air_conditioner_protocol.cpp)
target_link_libraries(test_air_conditioner_protocol PRIVATE libs_test_support libs_devices libs_warnings)
add_test(NAME air_conditioner_protocol COMMAND test_air_conditioner_protocol)

add_executable(test_air_conditioner_device tests/test_air_conditioner_device.cpp)
target_link_libraries(test_air_conditioner_device PRIVATE libs_test_support libs_transport libs_devices libs_warnings)
add_test(NAME air_conditioner_device COMMAND test_air_conditioner_device)

add_executable(test_device_actions tests/test_device_actions.cpp)
target_link_libraries(test_device_actions PRIVATE libs_test_support libs_devices libs_warnings)
add_test(NAME device_actions COMMAND test_device_actions)

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
# 与 build.ps1 的 libs_business 列表逐行一致。
# shared/ 先编（它是底座），再 readiness（业务一），再 measurement（业务二），
# 最后 app/（进程边界与装配）。顺序即依赖方向。
add_library(libs_business STATIC
  # shared/：共同底座。**不许反向依赖 readiness/ 或 measurement/**
  # （build.ps1 里有对应静态检查；CMake 侧靠源文件清单本身可见）。
  src/business/shared/time_util.cpp
  src/business/shared/json/json.cpp
  src/business/shared/config/site_config.cpp
  src/business/shared/config/parameter_store.cpp
  src/business/shared/session/device_session.cpp
  src/business/shared/logging/run_log.cpp
  src/business/shared/data_delivery.cpp
  # readiness/：业务一，就绪判定。
  src/business/readiness/facts.cpp
  src/business/readiness/verdict.cpp
  src/business/readiness/lamp.cpp
  src/business/readiness/monitor.cpp
  src/business/readiness/precheck.cpp
  # measurement/：业务二，激光触发光谱检测。
  src/business/measurement/frame.cpp
  src/business/measurement/frame_bus.cpp
  src/business/measurement/writer.cpp
  src/business/measurement/capture_plan.cpp
  src/business/measurement/job.cpp
  # app/：进程边界与装配。ParameterService 在这儿而不是 shared/，
  # 因为它要引用 measurement 的参数类型。
  src/business/app/parameter_service.cpp
  src/business/app/runtime.cpp
  src/business/app/facade.cpp
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

# IPC 测试直接调 handle_line，不真的建管道，所以不拖 main.cpp。
add_executable(test_business_ipc tests/test_business_ipc.cpp src/businessd/named_pipe_server.cpp)
target_link_libraries(test_business_ipc PRIVATE libs_test_support libs_business advapi32 libs_warnings)
add_test(NAME business_ipc COMMAND test_business_ipc)


# 帧队列：纯内存，无设备依赖。通道三态、背压丢最旧、订阅生命周期。
# 它只链 frame/frame_bus 两个纯内存源文件，**不链 libs_devices 也不链
# libs_business** —— 真往里加设备调用会直接链接失败，这条纪律由 build 强制保证。
add_executable(test_frame_queue
  tests/test_frame_queue.cpp
  src/business/measurement/frame.cpp
  src/business/measurement/frame_bus.cpp
  src/business/shared/json/json.cpp
  src/business/shared/time_util.cpp
)
target_link_libraries(test_frame_queue PRIVATE libs_test_support libs_warnings)
add_test(NAME frame_queue COMMAND test_frame_queue)

# 测量作业：门禁拦截、wait_for_laser_trigger 恒真、逐发采集、
# 落盘队列有界不反压、停止无条件收光。全部离线，用采集替身。
add_executable(test_measurement_job tests/test_measurement_job.cpp)
target_link_libraries(test_measurement_job PRIVATE libs_test_support libs_business libs_warnings)
add_test(NAME measurement_job COMMAND test_measurement_job)
if(TARGET fake_avaspec)
  add_dependencies(test_measurement_job fake_avaspec)
  set_tests_properties(measurement_job PROPERTIES ENVIRONMENT "LIBS_FAKE_AVASPEC_DLL=$<TARGET_FILE:fake_avaspec>")
endif()
