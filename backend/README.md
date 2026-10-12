# Rust 后端

上游基线为 `fnb666888/LIBS-device` 的 main 提交 `351b17c7d243870e791450d8c1355acb5040e165`，版本 v1.1。此目录是新的后端实现；原 `src/src/` C++ 源码保留用于接口对照和离线测试，不参与 Rust 后端运行。Qt 界面继续通过原命名管道和 JSON 接口访问后端。

## 获取测试分支

Rust 重构位于 `feature/rust-backend` 分支。建议拉取到独立目录，按下方要求安装构建工具后进行测试：

```powershell
git clone --branch feature/rust-backend --single-branch https://github.com/fnb666888/LIBS-device.git LIBS-device-rust
cd LIBS-device-rust
```

首次构建需要联网下载 Rust 依赖。仓库不包含本机工具链、编译产物、厂家 SDK DLL 和完整现场模型；离线启动及软件测试无需连接真实设备。

## 构建与启动

需要 Windows x64、Rust stable 和 rustfmt。推荐 Windows MSVC 工具链及 Visual Studio C++ Build Tools；GNU 工具链需要完整 MinGW，构建脚本兼容本工作区中的本地工具链及中文路径。光谱仪替身测试还需要 g++，无 g++ 时该测试明确跳过，其他测试仍运行。

在仓库根目录构建：

```powershell
.\build.ps1 -Release
```

产物为 `backend/target/release/businessd.exe`。离线启动不会打开硬件：

```powershell
.\backend\target\release\businessd.exe --no-devices --data-dir .\backend\data
```

原界面使用 `\\.\pipe\libs-businessd`，因此可以连接这个程序。现有 `src/remote/lpc.py` 命令工具仍兼容；它只是客户端，不属于后端运行依赖。连接同一管道时只运行一个守护进程。

设备运行使用原有装机配置与参数文件：

```powershell
.\backend\target\release\businessd.exe --config .\site.json --data-dir .\data --algorithm-dir .\src\algorithms
```

`--print-default-config` 输出配置模板。算法目录也可通过 `LIBS_ALGORITHM_DIR` 指定，命令行优先。安装发行目录时，放置 `businessd.exe` 与同级 `algorithms/`，或显式指定算法目录，避免依赖编译机器路径。

## 实现范围

| 部分 | Rust 实现 |
| --- | --- |
| 后端进程与接口 | 17 个原业务命令、Windows 异步命名管道、结构化错误、配置、日志、参数保存、恢复快照 |
| 设备与安全 | 激光器串口协议、状态灯、就绪门禁、准备/布防/开闸顺序、周期重判、自动停止与读回 |
| 光谱仪 | 直接通过 C ABI 加载厂家 AvaSpec DLL，包含多通道同步配置、准备缓存、取消和清理 |
| 测量 | 连续采集、共享帧、订阅队列、磁盘队列、停止时排空与分离的预测线程 |
| 模型 | 原铁矿预处理、ExtraTrees 预测、域外判断与结果结构，运行时不启动 Python |
| 算法扩展 | 完整算法包替换、免定标/定标模式、参数透传、版本验证、进程超时与在线重载 |
| 备用设备库 | TOF101 测距仪及空调 Modbus 读写、寄存器解释、设定值和模式保持 |

当前 main 的业务流程只装配激光器、状态灯和光谱仪。测距仪与空调作为 Rust 库保留，未额外加入 main 已删除的运行流程或命令。厂家 SDK 二进制仍由厂家提供，Rust 适配层不更改 DLL。

每个订阅队列最多 8 帧，落盘队列最多 64 帧；积压时丢最旧项并计数，不阻塞采集。原始帧用 Arc 共享，波长标定缓存复用，SDK 准备配置在参数未变时复用。发行构建开启 thin LTO；这里没有真实设备吞吐量基准，不据此声称性能提升倍数。

## 算法与模型

完整协议、更新和回滚步骤见 [ALGORITHM_INTERFACE.md](ALGORITHM_INTERFACE.md)。可编译的 Rust 插件模板为 `examples/algorithm_plugin.rs`，它明确返回未实现状态，不生成假浓度。

上游仓库只有模型清单和特征窗口，没有五个训练模型的完整 joblib 文件。安装实际模型后，只在转换时使用 Python：

```powershell
python .\backend\tools\export_model.py <完整模型目录> --output <Rust模型目录>
```

将原生引擎 `model_dir` 指向输出目录，执行 `prediction reload`。转换保留原树阈值、叶子值与标准化参数；输出与模型资产的 SHA-256 一起加载校验。缺少模型时如实返回不可用，不使用合成模型代替现场模型。

## 验证

构建脚本运行 Rust 测试，包括 DLL 字节布局、缓存、触发方向、取消与重启、设备错误和测量门禁失锁。算法接口测试使用可控测试进程，覆盖两种模式、参数传递、崩溃、超时、读写阻塞取消、版本更新和旧任务作废。

有原 C++ 基线产物时，可运行实际管道上的行为对照：

```powershell
python .\backend\tools\differential.py
python .\backend\tools\model_parity.py
python .\backend\tools\plugin_smoke.py
```

`differential.py` 对比 48 个离线请求及 300 次并发请求；排除不同进程的时间和临时目录。`model_parity.py` 生成测试森林，与原 Python 实现对比 34 组完整结果，浮点容差 2e-10，包含两分支、完整/裁切波段、重复波长拼接、低信噪比与无效输入。后者需 numpy、scipy、scikit-learn、pandas、joblib；这些只用于验证和导出。

软件模拟通过不等同于真实设备验收。真实 AvaSpec DLL、串口硬件、完整现场模型及标准样仍需在设备上验证；现有 Qt 界面的操作路径通过原协议保留，未进行真实硬件联调。
