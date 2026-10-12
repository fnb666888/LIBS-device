# LIBS 算法接入规范

本规范适用于 Rust 后端。算法包可以整体替换免定标算法、定标算法、预处理流程、模型与系数，不需要修改采集代码或重新编译守护进程。算法以常驻本地进程运行；推荐使用 Rust 编写，也可以接入其他语言生成的可执行程序。后端本身不依赖 Python。

## 1. 分工与兼容性

- 后端负责采集、设备控制、原始光谱传递、输入结构校验、任务调度、超时、取消、版本核对与结果保存。
- 算法负责通道拼接、背景处理、峰提取、质量判断、免定标计算、定标计算、模型/系数加载及算法参数校验。
- 后端不替插件归一化、补零、扣基线或修改波长，不能根据配置把 `hardware_synchronized=false` 改成 `true`。
- 算法不控制激光器、光谱仪、串口或后端的采集状态，不重新采集输入光谱。
- 保留原有 `libs.algorithm.v1` 输入和 `libs.algorithm.result.v1` 输出；原有 `prediction contract/validate/analyze/status/latest/configure` 调用继续可用。
- `prediction interface` 查询本扩展协议；`prediction reload` 重新加载当前配置、算法进程及整个模型包。旧 `contract` 返回原版基础契约，扩展能力以 `interface` 和本文件为准。

现有原生铁矿 ExtraTrees 引擎是一个独立默认实现，并不是所有算法共用的预处理层。其参数仍为显式 `branch=oxide|sulfide`。它不冒充免定标算法。选择外部引擎后，可完整替换它。

## 2. 安装算法包

在 `src/algorithms/runtime.local.json` 中配置：

```json
{
  "engine": "external",
  "executable": "packages/my-algorithm/2.0.0/libs-algorithm.exe",
  "args": ["--package", "packages/my-algorithm/2.0.0"],
  "working_directory": ".",
  "timeout_ms": 60000
}
```

`executable`、`working_directory` 的相对路径以算法目录为基准。`args` 是参数数组，直接传给进程，不经过 shell；参数中的路径按插件自己的规则解释。默认工作目录为算法目录。可选 `executable_sha256` 是可执行文件的 64 位十六进制 SHA-256；配置后，哈希不符会拒绝启动。模型与系数文件的哈希应由插件在加载时核验，并通过 `describe` 返回。

后端优先读取 `runtime.local.json`，否则读取 `runtime.json`。算法目录由守护进程 `--algorithm-dir` 或 `LIBS_ALGORITHM_DIR` 指定；实际选项见 `businessd --help`。原生模型目录的 `LIBS_MODEL_DIR` 仅用于原生引擎。

`timeout_ms` 必须为 1000 至 120000 的整数，同时约束进程握手、请求写入和一次计算。每次超时会终止该算法进程及其子进程；后续任务会重新加载。算法不得将日志写到 stdout；日志写 stderr 或自己的日志文件。后端在 Windows 上隐藏插件控制台窗口。

回到原生引擎：

```json
{
  "engine": "native",
  "model_dir": "iron_ore/model/current",
  "timeout_ms": 60000
}
```

原生模型先由 `tools/export_model.py` 一次性转换为 Rust 模型格式。旧配置的 `python` 字段不参与 Rust 原生运行。

## 3. 进程通信

编码为 UTF-8，无 BOM；每行一个 JSON 对象，以 LF 结束，插件也可输出 CRLF。stdout 仅含协议响应，每个请求恰好一个响应。单个插件响应最大 8 MiB。请求按顺序执行，一个进程同时只有一个计算任务；插件可自行在任务内部并行。

后端启动后先发送 `describe`，完成后发送任意多个 `analyze`：

```json
{"protocol":"libs.algorithm.worker.v1","id":1,"action":"describe"}
```

```json
{"protocol":"libs.algorithm.worker.v1","id":2,"action":"analyze","input":{"schema_version":"libs.algorithm.v1","request_id":"sample-42","parameters":{"mode":"calibration_free"},"spectrum":{"spectrum_source":"LIBS","spectrum_format":"wavelength_nm+counts","scan_groups":[{"shot_id":1,"complete":true,"hardware_synchronized":false,"channels":[{"serial":"S1","pixels":3,"wavelength_nm":[200,201,202],"counts":[10,20,15]}]}]}}}
```

以上 3 点光谱只用于展示结构，不满足实际定量算法的波段要求。

成功响应：

```json
{"protocol":"libs.algorithm.worker.v1","id":1,"ok":true,"payload":{}}
```

失败响应：

```json
{"protocol":"libs.algorithm.worker.v1","id":2,"ok":false,"error":{"code":"calibration_profile_missing","message":"Requested calibration profile is unavailable"}}
```

必须原样回传 `protocol` 和数值 `id`。`id` 属于进程通信；`input.request_id` 属于业务关联，两者不可混淆。非法 JSON、错配 ID、崩溃、输出超限或超时都进入失败状态，不会产生可用浓度。进程在 stdin EOF 时退出；后端关闭或重载可直接终止进程，不保证另发退出请求。插件不得把必要持久化工作留到退出时执行。

## 4. describe 响应

`payload` 至少包含：

```json
{
  "availability": "available",
  "implemented": true,
  "model_id": "my-libs-algorithm",
  "version": "2.0.0",
  "input_schema": "libs.algorithm.v1",
  "output_schema": "libs.algorithm.result.v1",
  "modes": ["calibration_free", "calibrated"],
  "parameter_schema": {
    "type": "object",
    "required": ["mode"],
    "properties": {
      "mode": {"enum": ["calibration_free", "calibrated"]},
      "calibration": {"type": "object"}
    }
  },
  "asset_sha256": {},
  "validated_for_quantification": false
}
```

`model_id`、`version` 必须为非空字符串；`modes` 至少支持一种模式。只实现一种算法时只声明该模式。`parameter_schema` 为可选 JSON Schema，由插件维护和执行校验，后端不自动执行该 Schema。波段要求、输出目标、单位、标定数据版本、适用材料与验证状态建议在描述中明确返回。加载失败时返回错误或退出，不能假报模型可用。

## 5. 输入规则

| 字段 | 规则 |
| --- | --- |
| `schema_version` | 固定 `libs.algorithm.v1` |
| `request_id` | 非空字符串，结果原样回传 |
| `context` | 可选对象，包含业务上下文；不从光谱猜测缺失业务字段 |
| `parameters` | 对象，外部算法必须显式指定 `mode` |
| `parameters.mode` | `calibration_free` 或 `calibrated`；必须被 describe 声明支持 |
| `model` | 可选对象；插件负责检查选定模型/配置的有效性 |
| `spectrum` | 原始 LIBS 光谱对象；兼容 `raw_capture` 别名 |
| `scan_groups` | 非空数组，每组至少一个真实通道 |
| `shot_id` | 可选非负精确整数 |
| `complete`、`hardware_synchronized` | 可选布尔值；只能依据采集事实解释 |
| `channels[].serial` | 同组内唯一的非空字符串 |
| `wavelength_nm`、`counts` | 非空有限数值数组，长度相同；波长必须大于零 |
| `pixels` | 可选；提供时必须为等于数组长度的正整数 |
| `device_time_ticks` | 可选非负精确整数 |

后端校验结构及数值有效性；插件还必须校验波长单调性、所需覆盖范围、信噪比、缺失通道及同步要求。负强度允许表示原始观测，不应被后端静默截断。不要把缺失通道补成全零谱。

`parameters` 对外部插件完整传递，后端不硬编码氧化矿/硫化矿，也不限制算法专有参数。推荐的定标参数：

```json
{
  "mode": "calibrated",
  "calibration": {
    "profile_id": "ore-reference-2026-10",
    "version": "3",
    "factor": 1.25
  }
}
```

上面的 `factor` 仅演示可传参数，不规定真实定标模型。曲线、系数、标准样或训练数据可位于算法包中；通过明确的 profile ID 选择。缺少定标数据必须返回错误或不可用结果，禁止回退到另一种模式。免定标算法需要的温度、电子密度、谱线库及物理假设也由插件自己的参数规范定义。

## 6. analyze 输出规则

成功通信不等于得到可用浓度。响应 `ok=true` 的 `payload` 至少包含以下结构：

```json
{
  "schema_version": "libs.algorithm.result.v1",
  "request_id": "sample-42",
  "mode": "calibration_free",
  "availability": "unavailable",
  "executed": false,
  "status": {"state": "not_executed", "reason": "insufficient_wavelength_coverage"},
  "composition": {
    "availability": "unavailable",
    "unit": "mass_fraction_percent",
    "unit_confirmed": false,
    "values": {},
    "target_values": {}
  },
  "quality": {"status": "review_required", "issues": ["insufficient_wavelength_coverage"]},
  "model": {
    "model_id": "my-libs-algorithm",
    "version": "2.0.0"
  }
}
```

结果必须匹配输入 `request_id` 和 `mode`；`model.model_id/version` 必须与本进程的 `describe` 相同。`availability` 为 `available` 或 `unavailable`，并与 `composition.availability` 一致。可用结果必须 `executed=true`、`unit_confirmed=true`，单位必须为 `mass_fraction_percent`，例如质量分数 0.25 应输出 25，而不是 0.25。其他单位应在插件内明确换算，无法确认时输出不可用。

`composition.values` 和 `target_values` 必须为对象，各值只能为有限数字或 null，不得为字符串、NaN 或 Infinity。界面兼容键包括 `COMPO_AL2O3`、`COMPO_CAO`、`COMPO_FEO`、`COMPO_MGO`、`COMPO_MNO`、`COMPO_P2O5`、`COMPO_S`、`COMPO_SIO2`、`COMPO_TFE`、`COMPO_TIO2`。未预测的目标用 null 或省略，不能用 0 冒充已测浓度。`target_values` 可增加元素或其他目标。

元素 P 与 P2O5、元素 Fe 与 FeO 等口径不得混用，换算必须明确声明。需记录标定 profile/版本、适用范围、异常标志及是否通过定量验证。建议沿用原契约的 `results`、`aggregation`、`classification`、`similarity`、`preprocessing`、`context` 与 `inference_ms` 字段，方便分析与审计。

后端会附加 `validation`、`source`、`context`、`branch`、`saved`、`result_path`，这些键保留给后端；算法不要赋予它们其他含义。保存文件中应包含足够的算法/模型/标定版本信息以复现计算。

## 7. 调用与更新

离线任务：

```json
{"command":"prediction","args":{"action":"analyze","input":{"schema_version":"libs.algorithm.v1","request_id":"sample-42","parameters":{"mode":"calibration_free"},"spectrum":{"spectrum_source":"LIBS","spectrum_format":"wavelength_nm+counts","scan_groups":[{"channels":[{"serial":"S1","wavelength_nm":[200,201,202],"counts":[10,20,15]}]}]}}}}
```

返回 `accepted/request_id` 后，轮询 `prediction status/latest` 获取最终结果。仅提交原始光谱的 `validate` 命令不执行算法。

实时任务参数：

```json
{"command":"prediction","args":{"action":"configure","enabled":true,"parameters":{"mode":"calibrated","calibration":{"profile_id":"ore-reference-2026-10","version":"3"}}}}
```

实时预测只消费测量流程产出的光谱帧。禁用：`{"command":"prediction","args":{"action":"configure","enabled":false}}`。采集与算法是独立流程；更新算法不会控制激光器或停止采集。

完整更新顺序：

1. 将新可执行程序、模型、系数和谱线库放进新的版本目录，不覆盖正在运行的文件。
2. 核验新包，原子替换 `runtime.local.json`，使配置指向新目录。
3. 调用 `{"command":"prediction","args":{"action":"reload"}}`。
4. 轮询 `status`，核对 `model.model_id/version`；加载失败时状态为 `model_unavailable`。
5. 用标准样或参考光谱确认新算法结果，再配置实时参数。切换免定标/定标模式应显式重新配置参数。

重载取消旧算法任务、清除待处理任务和旧 latest，并终止旧进程。旧版本迟到的结果不会写入新的 latest。加载失败不会静默沿用旧算法；已有测量采集继续独立运行。回滚时把配置指向旧版本目录，再次 reload。旧结果文件作为历史记录保留。

## 8. Rust 模板与验收

`examples/algorithm_plugin.rs` 是可编译的协议模板，默认返回 `algorithm_not_implemented`，不生成虚构浓度。替换其 `analyze` 函数，并在 `describe` 中声明实际算法及参数。构建命令：

```powershell
cargo build --release --example algorithm_plugin
```

把生成的 `target/release/examples/algorithm_plugin.exe` 放进算法包并更新配置。模板采用同一 Cargo 项目方便示范；正式插件可以独立项目编译，只需遵守此协议。

验收至少覆盖：两种实际支持的模式、真实参考光谱、标准样误差、质量不合格、缺失定标 profile、参数非法、波段不足、版本一致性、进程崩溃、超时、请求写入阻塞、重载中任务取消及回滚。`tests/algorithm.rs` 验证接口和生命周期，使用的测试算法只用于协议测试，不能用于设备定量。
