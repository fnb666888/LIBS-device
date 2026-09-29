#pragma once

// 成分分析（能力 11）。
//
// 判定依据：范围报告 §3.11
//   11.1 成分结果接口（10 字段 + 不可用态）[B]
//   11.2 成分算法实现 [C]
//
// **本文件存在的全部理由是那条禁令**（任务书 §5.2）：
//   没有算法时必须返回 unavailable，**double 字段不得有"看起来像真实值"的默认值**。
// 因此十个字段全部是 `std::optional<double>`，且外层有独立的 availability。
//
// 另一条容易踩的坑：§5.1 说"4组以上"，但**计数口径未定义**——
// 不得把"4 组"推定为 4 Hz，也不得推定为四路各出一份。
// 本接口只表达"一组结果"，不表达发布周期，也不表达有几路。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "libs/business/json/json.hpp"
#include "libs/business/time_util.hpp"
#include "libs/error.hpp"

namespace business::services {

// 任务书 §5.1 的十项字段。顺序与文档一致，输出时也按这个顺序。
enum class Oxide {
    TiO2, TFe, SiO2, S, P, MnO, MgO, FeO, CaO, Al2O3,
};
const char* to_string(Oxide oxide) noexcept;
std::vector<Oxide> all_oxides();

// 结果可用性。**三态**：有结果 / 不可用 / 算法未实现。
// "不可用"（这一组没算出来）与"算法未实现"是**两回事**，不能合并。
enum class CompositionAvailability {
    available,        // 这一组有有效结果
    unavailable,      // 这一组没算出来（例如缺少光谱数据）
    algorithm_missing // 算法根本没实现（当前恒为此值）
};
const char* to_string(CompositionAvailability availability) noexcept;

// 一组成分结果。**每个字段都是 optional：没有就是没有。**
struct CompositionResult {
    CompositionAvailability availability = CompositionAvailability::algorithm_missing;
    Milliseconds computed_at_ms = 0;
    std::string source;                   // 关联的采集引用（shot_id / 路径）

    // 十个字段，缺失即 nullopt。**不要改成 double + 有效位标志**——
    // 那正是任务书 §5.2 禁止的"看起来像真实值"的默认值。
    std::map<Oxide, std::optional<double>> values;

    static CompositionResult algorithm_missing_result(std::string source) {
        CompositionResult r;
        r.availability = CompositionAvailability::algorithm_missing;
        r.source = std::move(source);
        r.values.clear();
        return r;
    }
};

// 序列化。不可用的字段输出 null，**不输出 0**。
json::Value to_json(const CompositionResult& result);

// 成分分析器。
//
// 当前**不提供任何算法实现**（11.2 C 类）。所有分析入口返回
// algorithm_missing_result，**而不是一个全 0 的假结果**。
// 这一对子项是合并最有说服力的例子：接口正是算法的输出契约，
// 算法落地时接口不需重写。
class CompositionAnalyzer {
public:
    // 分析一组光谱。**当前恒返回 algorithm_missing。**
    // 未来算法落地后，这里会真正计算，但仍必须对缺失的通道返回 unavailable
    // 而不是 0。
    CompositionResult analyze(const std::string& source) const;

    bool algorithm_implemented() const noexcept { return false; }

    static std::string prerequisites();
};

}  // namespace business::services
