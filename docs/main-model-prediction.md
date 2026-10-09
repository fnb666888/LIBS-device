# 主分支模型预测接入

## 使用

主界面右侧增加“成分预测”。显式选择“氧化矿”或“硫化矿”后，后端加载对应模型，实时接收后续测量帧。选择“关闭预测”清空当前预测结果；这不会停止光谱采集。后端重启后默认关闭预测，需要重新选择矿种。

“导入 LIBS 光谱预测”支持两列数值文件（空白、逗号或分号分隔）：第一列波长 nm，第二列原始强度 counts。允许空行和 `#` 注释，至少三个点、最多十万个点，文件不超过 16 MB。先选择矿种，再导入文件。该操作不启动激光器；导入文件不声明硬件同步。

输出为 TFe、SiO2、Al2O3、TiO2、CaO、MgO、MnO、P、S、FeO，单位为质量分数 %。P 为元素磷，不能直接当作 P2O5；FeO 所含铁已经包含在 TFe 内，不能再次相加。沿用原模型的研究预测状态和域外、物理约束标记，不把模型输出宣称为已验证的定量结果。

## 新模型与真机波段适配

当前安装的是 `D:\桌面\铁矿光谱预测` 在 2026-10-09 新训练的 `current_mixed_v2` 模型，模型 ID 为 `iron_ore_current_mixed_v2`。训练网格为 200–565 nm、步长 0.05 nm，120 个谱窗和两种积分宽度生成 240 个特征。五个模型文件与谱窗、manifest 按原始字节复制，加载时核对哈希。

2026-10-09 通过 `myserver5` 真机读回的四通道为：185.62–312.09、305.82–415.88、403.73–496.66、488.82–564.73 nm，每通道 2048 点。本次通道间连续，无历史记录所描述的 496.7–512 nm 缺口；应以 SDK 实际返回的波长数组为准。

完整覆盖 200–565 nm 的输入保留原项目预处理。真机末端不足 565 nm 时，v2 使用固定的 200–564.7 nm 网格，覆盖全部特征窗口（最高窗口上边界约 564.648 nm）。这项适配不补零、不外推，低于 564.7 nm 或存在超过 1 nm 波段缺口时仍拒绝预测。结果记录训练范围、实际网格、`model_boundary_adapted`，界面显示末端适配提示。

缩短网格会改变基线及噪声估计，不能视为与完整网格完全等价。在 80 条已有留出谱上的对比，最大单项预测变化为 FeO 的约 0.2545 个百分点。适配和域外结果用于研究观察，尚未完成真机定量精度验证。旧 v1 仍需要完整 200–600 nm，不能套用 v2 的末端适配。

## 架构

- `src/algorithms/`：Python 模型运行代码、特征提取与模型资产；保留原训练模型和完整网格推理，新模型支持上述明确记录的末端适配。
- `business/measurement/prediction/`：算法契约、常驻 Python 进程适配、异步预测服务。服务订阅 `FrameQueue`，独立于 UI 订阅和原始数据写盘。
- `Runtime`：装配预测服务，在设备测量停止后取消模型任务并回收工作进程。
- `BusinessFacade`：新增 `prediction` 命令；配置和状态只访问内存，离线预测提交后立即返回。
- UI：通过原命名管道读取状态与结果，不直接加载模型、不接触设备驱动。

预测服务的队列继承帧队列的有界策略：默认八帧，满时丢最旧帧，模型慢不会反压设备采集。可观察到的帧序号缺口在 `skipped_frames` 中统计。它不是“每一发必有预测”的保证。失败采集帧和非激光测量帧不送模型；缺失通道不补零。切换矿种或关闭预测会使旧的未完成任务失效，不向界面回填旧分支结果。

预测成功与拒绝结果独立写入后端数据目录的 `predictions/`，包含请求 ID、来源、帧/样品上下文、模型资产哈希、分支和质量信息。原始光谱按原流程保存。预测保存失败与预测失败分别显示。

## 模型部署

本机已有五个 `.joblib` 和 Python 环境，配置在 `src/algorithms/runtime.local.json`。模型与本机配置不进入 Git。新机器可使用：

```powershell
pwsh -File src/algorithms/install_model.ps1 -SourceProject "D:\桌面\铁矿光谱预测" -PythonPath "C:\Python314\python.exe"
& C:\Python314\python.exe -m pip install -r src/algorithms/requirements.txt
```

模型加载校验 manifest 中全部资产的 SHA256。特征窗口 CSV 也属于哈希资产，`.gitattributes` 保留其原始字节，避免 Git 换行转换导致加载失败。

运行目录默认定位到守护进程旁的 `algorithms/`，或者构建目录上一级的 `algorithms/`。可用 `LIBS_ALGORITHM_DIR`、`LIBS_ALGORITHM_PYTHON` 覆盖。独立模型进程不访问网络；加载和推理等待可取消，守护进程退出时回收进程。

## 接口

| 命令 | 参数 | 返回 |
| --- | --- | --- |
| `prediction` | `action: status` | 配置、状态、计数、模型及最新结果；不触发加载 |
| `prediction` | `action: configure, enabled: true, branch: oxide/sulfide` | 异步加载并启用实时预测 |
| `prediction` | `action: configure, enabled: false` | 关闭实时预测并清空结果 |
| `prediction` | `action: contract/validate` | 契约或结构校验；校验通过不代表可预测 |
| `prediction` | `action: analyze, input: {...}` | 异步提交离线光谱，返回 `accepted` 和 `request_id`；忙时拒绝 |
| `prediction` | `action: latest` | 最新结果；无结果为 null |

输入沿用 `libs.algorithm.v1`，结果为 `libs.algorithm.result.v1`。通过 `request_id` 关联提交和结果；实时预测另带原始 `frame_index`、`shot_id` 与采集时间。详细输入结构可查询 `contract`。

## 验证

```powershell
pwsh -NoProfile -File src/build.ps1
pwsh -NoProfile -File ui/build.ps1 -QtPrefix D:\Qt6\6.11.2\mingw_64
$env:LIBS_MODEL_SOURCE='D:\桌面\铁矿光谱预测'
& src/algorithms/.venv/Scripts/python.exe -m pytest src/tests/test_model_prediction.py -q
& src/algorithms/.venv/Scripts/python.exe src/tests/verify_prediction_ipc.py --source $env:LIBS_MODEL_SOURCE --ui ui/build/libs-spectrum-ui.exe
```

Python 测试需要 `requirements-dev.txt` 中的 pytest。本次通过全部 16 套 C++ 测试（1631 项检查）、12 项 Python 模型测试和实际进程端到端验证。验证覆盖原项目实测留出光谱结果一致性（绝对误差容限 1e-10）、四通道拼接、分组预测与平均、末端适配及边界拒绝、旧 v1 不放宽范围、波段缺口、空白谱、模型哈希、异步帧订阅、慢预测、结果保存、矿种切换、并发 IPC、模型加载/预测中退出、Qt 矿种选择与文件导入、大小窗口排版。

旧 v1 的真机测试及波段拒绝记录见 [第一次真机联测记录](remote-hardware-verification-20261009.md)。新模型的完整网格、末端适配验证见 [新模型检查](new-model-verification-20261009.md)。v2 部署与实时预测结果见 [新模型部署记录](model-v2-deployment-20261009.md)。
