#include "libs/business/services/composition.hpp"

namespace business::services {

const char* to_string(Oxide oxide) noexcept {
    switch (oxide) {
        case Oxide::TiO2:  return "TiO2";
        case Oxide::TFe:   return "TFe";
        case Oxide::SiO2:  return "SiO2";
        case Oxide::S:     return "S";
        case Oxide::P:     return "P";
        case Oxide::MnO:   return "MnO";
        case Oxide::MgO:   return "MgO";
        case Oxide::FeO:   return "FeO";
        case Oxide::CaO:   return "CaO";
        case Oxide::Al2O3: return "Al2O3";
    }
    return "unknown";
}

std::vector<Oxide> all_oxides() {
    return {Oxide::TiO2, Oxide::TFe, Oxide::SiO2, Oxide::S, Oxide::P,
            Oxide::MnO, Oxide::MgO, Oxide::FeO, Oxide::CaO, Oxide::Al2O3};
}

const char* to_string(CompositionAvailability availability) noexcept {
    switch (availability) {
        case CompositionAvailability::available:          return "available";
        case CompositionAvailability::unavailable:        return "unavailable";
        case CompositionAvailability::algorithm_missing:  return "algorithm_missing";
    }
    return "algorithm_missing";
}

json::Value to_json(const CompositionResult& result) {
    json::Value v = json::object();
    v.set("availability", json::Value(to_string(result.availability)));
    v.set("computed_at_ms", json::Value(static_cast<double>(result.computed_at_ms)));
    v.set("source", json::Value(result.source));
    json::Value values = json::object();
    for (Oxide oxide : all_oxides()) {
        const auto it = result.values.find(oxide);
        if (it != result.values.end() && it->second.has_value()) {
            values.set(to_string(oxide), json::Value(*it->second));
        } else {
            // 不可用 -> null。**绝不输出 0**（任务书 §5.2）。
            // 但也不能静默省掉这个键：调用方需要能区分
            // "这项没算出来"（键存在、值为 null）和"这项不存在"（键不存在）。
            values.set(to_string(oxide), json::Value());
        }
    }
    v.set("values", std::move(values));
    v.set("note", json::Value("字段为 null 表示该指标未产出；不得把 null 读作 0"));
    return v;
}

CompositionResult CompositionAnalyzer::analyze(const std::string& source) const {
    // 前置条件：① 算法窗口定义 ② 十项指标的标定与验证方案
    //           ③ 精度要求 ④ “4组以上”的计数口径
    // 全部未确认（任务书 §4.1、§5.1、§5.2），因此这里不尝试任何计算。
    return CompositionResult::algorithm_missing_result(source);
}

std::string CompositionAnalyzer::prerequisites() {
    return "① 算法窗口定义 ② 十项指标的标定与验证方案 ③ 精度要求 "
           "④ “4组以上”的计数口径（任务书 §4.1/§5.1/§5.2 均未给出；范围报告 §3.11.2）";
}

}  // namespace business::services
