#pragma once

// LIBS 算法的可调用契约；本模块只校验输入、描述结构和返回缺失算法状态。
// 不访问设备、不联网、不推理、不把原始强度当成成分含量。
#include "libs/business/json/json.hpp"
#include "libs/error.hpp"

namespace business::services::algorithm {

// 可供 UI 加载的字段描述与空输入/输出模板。空输入模板不能通过校验。
json::Value contract();

// 语义更完整的别名，供门面路由使用；与 validate_input 完全相同。
libs::Result<json::Value> validate_algorithm_input(const json::Value& input);

// 参数是 args.input；成功返回输入摘要与可选信息的缺失说明。
// 必填结构或光谱数据非法时返回 invalid_argument，错误包含字段路径。
libs::Result<json::Value> validate_input(const json::Value& input);

// 先校验；合法输入也只返回 algorithm_missing，所有预测字段保持 null。
libs::Result<json::Value> analyze_placeholder(const json::Value& input);

}  // namespace business::services::algorithm
