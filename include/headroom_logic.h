#pragma once

#include <cstdint>

namespace vhb {

enum class PressureState { healthy, pressured };

struct Sample {
  std::uint64_t budget_bytes{};
  std::uint64_t usage_bytes{};
};

[[nodiscard]] std::uint64_t headroom_bytes(const Sample &sample);
[[nodiscard]] double headroom_percent(const Sample &sample);

// Hysteresis prevents repeated alerts when usage hovers around the threshold.
[[nodiscard]] PressureState next_pressure_state(PressureState current,
                                                const Sample &sample,
                                                double enter_percent,
                                                double exit_percent);

} // namespace vhb
