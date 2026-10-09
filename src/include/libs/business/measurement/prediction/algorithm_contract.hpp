#pragma once

// LIBS 算法契约与结构校验；实际模型推理由 ModelInference 服务执行。
#include "libs/business/shared/json/json.hpp"
#include "libs/error.hpp"

namespace business::measurement::prediction::algorithm {

// 可供 UI 加载的字段描述与空输入/输出模板。空输入模板不能通过校验。
json::Value contract();

// 语义更完整的别名，供门面路由使用；与 validate_input 完全相同。
libs::Result<json::Value> validate_algorithm_input(const json::Value& input);

// 参数是 args.input；成功返回输入摘要与可选信息的缺失说明。
// 必填结构或光谱数据非法时返回 invalid_argument，错误包含字段路径。
libs::Result<json::Value> validate_input(const json::Value& input);

}  // namespace business::measurement::prediction::algorithm
