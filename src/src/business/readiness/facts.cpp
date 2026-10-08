#include "libs/business/readiness/facts.hpp"

#include <cstdint>

namespace business::readiness {

LaserSafetyFacts parse_actual(const libs::devices::LaserActual& actual,
                              std::uint32_t* discharge_counter) {
    LaserSafetyFacts facts;
    facts.available = actual.valid;
    facts.work_state = actual.work_state == 1;
    facts.interlock_1 = actual.interlock_1 == 0;   // 1=断开=不安全
    facts.interlock_2 = actual.interlock_2 == 0;
    facts.emergency_stop = actual.emergency_stop == 0;
    facts.q_gate = actual.q_gate == 0;             // 1=开=不安全
    facts.work_enabled = actual.work_enabled == 1;
    facts.over_temperature = actual.overtemperature == 0;
    facts.fault_bits = actual.fault_bits;
    if (discharge_counter != nullptr) *discharge_counter = actual.discharge_counter;
    return facts;
}

const char* to_string(MachineState state) noexcept {
    switch (state) {
        case MachineState::offline:    return "offline";
        case MachineState::standby:    return "standby";
        case MachineState::ready:      return "ready";
        case MachineState::acquiring:  return "acquiring";
        case MachineState::fault:      return "fault";
        case MachineState::recovering: return "recovering";
    }
    return "unknown";
}

}  // namespace business::readiness