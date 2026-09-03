#include "headroom_logic.h"

#include <algorithm>

namespace vhb {

std::uint64_t headroom_bytes(const Sample &sample) {
  return sample.usage_bytes >= sample.budget_bytes
             ? 0
             : sample.budget_bytes - sample.usage_bytes;
}

double headroom_percent(const Sample &sample) {
  if (sample.budget_bytes == 0) {
    return 0.0;
  }
  return 100.0 * static_cast<double>(headroom_bytes(sample)) /
         static_cast<double>(sample.budget_bytes);
}

PressureState next_pressure_state(PressureState current, const Sample &sample,
                                  double enter_percent, double exit_percent) {
  const double percent = headroom_percent(sample);
  if (current == PressureState::healthy && percent <= enter_percent) {
    return PressureState::pressured;
  }
  if (current == PressureState::pressured && percent >= exit_percent) {
    return PressureState::healthy;
  }
  return current;
}

} // namespace vhb
