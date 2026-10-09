代码和五个模型来自用户指定的 `D:\桌面\铁矿光谱预测`，2026-10-09 更新为 `current_mixed_v2`：200–565 nm、120 个谱窗、240 维特征。

- `model_runtime.py`、`spectrum_features.py`、`project_utils.py` 复用源项目；完整 200–565 nm 输入保持源项目预测结果。
- `predict_worker.py` 对当前 v2 增加经过复核的末端适配：输入不足 565 nm 时，只接受完整覆盖 200–564.7 nm 的数据，按固定 564.7 nm 网格处理，确认全部 120 个完整谱窗仍在范围内。原始数据不补零、不外推；结果记录实际网格、`model_boundary_adapted` 和域外状态。这会改变末端基线，属于研究预测适配，不能宣称与原预处理完全等价。
- v1 备份仍要求完整 200–600 nm；末端适配不能用于旧模型缺失的谱窗。
- `model_runtime.py` 仅把化学常量导入改为 `model_definition.py`，避免推理导入训练用的物理仿真和联网模块。
- `model_definition.py` 的 TARGETS、Fe/O/S 原子质量、FeO 定义来自源项目 `training_physics/extended_physics.py` 和 `feo_extension/feo_chemistry.py`。
- 五个 `.joblib`、特征窗口 CSV 与 manifest 已复制到 `model/current`；模型和窗口保持原始 SHA256。
- 模型二进制为本机部署资产，Git 不跟踪。换机器可运行 `../install_model.ps1` 从源模型包安装。
- 所有输出均保留源项目“研究预测、未验证用于定量”的状态；P 为元素含量，不自动换算 P2O5。
