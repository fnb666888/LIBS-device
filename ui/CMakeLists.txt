# 实时光谱 UI（`ui/CMakeLists.txt`）
#
# 纪律（docs/architecture-and-workflows.md §4.2 的依赖方向）：
#   这个 target **只**链接 Qt6Widgets + Qt6Charts + Qt6Core。
#   它**不**链接 libs_devices，也**不**链接 libs_business。
#   原因不是洁癖：C++ 里 `libs_business` 对 `libs_devices` 是 PUBLIC 依赖，
#   PUBLIC 会传染到下游的链接命令行——链接 libs_business 就等于把设备层
#   拖进来，UI 就"能"直接碰设备了。判定权一旦漏到 UI，分层就名存实亡。
#   与业务层的唯一接口是**命名管道上的 JSON 行**（`ui/src/pipe_client.cpp`）。
#
# 编译：控制端编译 + 拷贝部署（与业务层同一条路径，见 docs §10.5）。
#   pwsh -File build.ps1
# 或 CMake：
#   cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=D:/Qt6/6.11.2/mingw_64
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(libs_spectrum_ui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)   # Q_OBJECT / signals-slots 必须开
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 全链路 MinGW。Qt6 官方包只提供 MinGW 构建，混 MSVC 会链接失败。
if(NOT MINGW)
    message(FATAL_ERROR "本工程要求 MinGW 工具链（与 D:/Qt6 的 mingw_64 匹配），当前编译器不是 MinGW")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Charts)

add_executable(libs-spectrum-ui WIN32
    src/main.cpp
    src/main_window.cpp
    src/parameter_page.cpp
    src/theme.cpp
    src/widgets.cpp
    src/machine_state.cpp
    src/status_poller.cpp
    src/stream_poller.cpp
    src/frame_model.cpp
    src/spectrum_plot.cpp
    src/pipe_client.cpp
)

target_link_libraries(libs-spectrum-ui PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Charts
)

# 与业务层同样的告警档位（-Wall -Wextra -Wpedantic）。
target_compile_options(libs-spectrum-ui PRIVATE -Wall -Wextra -Wpedantic)

# 现场部署便利：exe 和它依赖的 Qt6 DLL 一起丢进 bin/ 便于拷贝。
# （windeployqt 在 Windows 上按需跑，见 docs 的部署段。）
