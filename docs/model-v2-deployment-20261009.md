# 新模型真机部署与实时预测记录

2026-10-09 已将主分支当前工作区的新模型版本部署到 SSH `myserver5`，完成离线回放和短时激光采集预测。实时 4 帧均得到十项成分预测并保存；采集、写盘、预测失败和跳帧均为 0。测试结束后激光停止回读成功、服务退出码 0、状态灯各色和蜂鸣器均关闭。

## 启动与保存位置

- 真机部署目录：`C:\Users\user\libs\main-model-v2-20261009`。
- 真机桌面入口：`C:\Users\user\Desktop\LIBS 新模型预测.lnk`。
- 启动脚本：部署目录内 `launch-ui.ps1`，启动设备服务和交互式 Qt 界面，默认启用氧化矿预测；不自动开始激光测量。
- 使用顺序：关闭其他占用仪器的程序，打开上述入口，点击“设备预检”，按实际样品选择氧化矿或硫化矿，点击“开始检测”。结束时点击“停止检测”；关闭窗口时启动脚本也会请求停止并关闭服务，要求确认停止回读。
- 原始数据和预测结果：部署目录内 `data-hardware`；预测 JSON 在 `data-hardware\predictions`，含模型哈希、矿种、帧号、实际处理网格及质量标记。
- 旧部署 `C:\Users\user\libs\verify-main-20261009-a37c` 保留；本次使用独立目录和命名管道 `libs-main-model-v2-20261009`，无后台采集常驻。

直接启动 `ui\libs-spectrum-ui.exe` 不负责启动服务，应使用桌面入口。主分支服务本身仍在重启后默认关闭预测；上述现场启动脚本明确启用氧化矿默认值，操作者可在界面切换矿种。

## 模型与边界处理

来源为 `D:\桌面\铁矿光谱预测\model\current` 在 2026-10-09 训练的新模型，格式 `current_mixed_v2`，运行 ID `iron_ore_current_mixed_v2`。五个 `.joblib`、manifest、谱窗按原始字节部署，远端逐项 SHA256 校验通过。没有重新训练模型，也没有改动源训练项目。

训练网格 200–565 nm，步长 0.05 nm，120 个窗口、240 个特征。真机光谱截止 564.729309 nm，运行时选择固定 200–564.7 nm 网格，保留全部积分窗口；完整波段输入仍使用原完整网格。不补零、不外推，低于 564.7 nm 或超过 1 nm 的光谱缺口仍拒绝。结果和界面明确记录 `model_boundary_adapted`。

缩短网格会影响基线与噪声估计，这不是完整网格结果的精度保证。80 条留出谱上的同模型对比、旧 v1 不能放宽缺失波段的原因见 [新模型检查记录](new-model-verification-20261009.md)。

部署完整性：

| 文件 | SHA256 |
| --- | --- |
| manifest.json | `09a502d6c548390e6babf71ed7dfbb7b218a0c34fc414c9e4ef193f012ea7dfe` |
| businessd.exe | `884ff38ca75f29e1b7b69e493eb175c1ea13a929fc0a4549da8c5d8363d00e34` |
| libs-spectrum-ui.exe | `7da3334ed0b0722a85c03b2c957e6ff40f7d0626471e2aa4755137896c5f1d22` |
| model-v2-deployment.zip | `6e158f8b88324642eb4fa3da376ddc6e2d928d5c1196359738da54cdd60e0cc0` |

远端 Python 3.14.6，NumPy 2.5.3、SciPy 1.18.1、scikit-learn 1.9.1、pandas 3.0.6、joblib 1.5.3；Qt 6.11.2。独立 Python 环境放在部署目录的 `python`，推理不需要网络。

## 验证

本机已通过 16 套 C++ 测试（1631 项检查）、12 项 Python 测试，以及实际 IPC、异步预测保存、矿种切换、模型加载/推理取消、Qt 文件导入和大小窗口检查。

远端首先使用 `--no-devices` 验证：完整波段基准谱与源项目十项输出误差不超过 1e-10；此前保存的 4 帧真机光谱与独立末端适配诊断的十项结果误差不超过 1e-10，全部生成并保存结果，Qt 启动与正常退出通过。

现场已授权短时出光，本次沿用已验证参数：激光器 COM10、38400/8N1，80 A、200 μs、1 Hz、Q 延时 210 μs；光谱仪积分 1.05 ms、积分延时 1280 ns、采集超时 10000 ms。COM8 状态灯型号 CL50S+K50-485，9600/8E1、地址 1。测量前预检及灯色回读通过，独立 35 秒超时停止脚本先启动，再开始测量；实际约 5 秒后请求停止。

| 检查项 | 结果 |
| --- | --- |
| 四通道光谱 | 每帧四台均返回 2048 点，共 4 帧 |
| 波段 | 185.617–312.090、305.815–415.876、403.731–496.661、488.820–564.729 nm |
| 实时预测 | 4/4 完成，来源 `live`，模型为 v2，十项均有限数值 |
| 原始光谱写盘 | 成功 4、失败 0、积压 0 |
| 预测结果写盘 | 4 个 JSON，帧号 0–3，与采集帧对应 |
| 预测失败/输入拒绝/跳帧 | 0 / 0 / 0 |
| 单帧推理时间 | 253–421 ms |
| 停止回读 | 成功，耗时 359 ms，测量与激光会话均停止 |
| 服务和状态灯 | 服务正常退出 0；独立串口查询确认红黄绿及蜂鸣器关闭 |
| 最终进程检查 | 本次设备服务、Qt、Python 工作进程均无残留 |

现场这次使用氧化矿分支验证程序链路，不据此判定现场样品矿种。本次四帧全部 `outside_observed_domain=true`，同时保留 `hardware_synchronization_not_confirmed`。设备能完成激光触发四通道采集，但逐发硬件同步证据尚未确认。这些结果是研究预测，尚未验证真机定量精度。

## 最后一帧的研究预测

帧号 3，质量分数 %，P 为元素磷；FeO 的铁已包含在 TFe 中。未经真实化学参考值验证。

| 成分 | 预测值 % |
| --- | ---: |
| TFe | 42.422688 |
| SiO2 | 23.997307 |
| Al2O3 | 6.175007 |
| TiO2 | 4.123340 |
| CaO | 1.100433 |
| MgO | 0.710155 |
| MnO | 0.138084 |
| P | 0.013165 |
| S | 0.022079 |
| FeO | 14.373303 |

## 证据

本机路径 `D:\LIBS\build\model-deployment\20261009-v2\`：

- `backend-build.log`、`ui-build.log`、`model-tests.log`、`ipc-check.log`：构建及本机测试。
- `backup-iron-ore`、`backup-predict-worker.py`：替换前的本机 v1 备份。
- `remote-results\verification-results.zip`：远端完整证据；解压目录 `remote-results\evidence`。
- `remote-results\evidence\hardware-summary.json`、`hardware-prediction.json`、`verification-final.json`：实时计数、最新预测、四个已保存预测和最终进程状态。
- `remote-results\evidence\hardware-frames.json`、`data-hardware`：此次原始光谱及预测文件。
- `remote-results\evidence\offline-summary.json`、`offline-hardware-replay.json`：离线基准和四帧回放。
- `remote-results\ui-live.png`：真机 Qt 界面、四通道光谱和十项预测。

![真机实时预测界面](../build/model-deployment/20261009-v2/remote-results/ui-live.png)

## 后续采集异常

上述哈希和结果是首次短时联测时的记录。用户后续 UI 检测出现第四台光谱仪在 SDK 中缺失的异常，已修复掉线后的高速重试并更新同目录后端，保留原后端备份；模型、前端与生产参数保持原部署版本。排查与当前后端哈希见 [采集异常记录](capture-disconnect-investigation-20261009.md)。
