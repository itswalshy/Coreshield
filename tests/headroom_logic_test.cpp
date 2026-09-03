#include "headroom_logic.h"

#include <cassert>
#include <cmath>

int main() {
  using vhb::PressureState;
  using vhb::Sample;

  assert(vhb::headroom_bytes(Sample{100, 25}) == 75);
  assert(vhb::headroom_bytes(Sample{100, 150}) == 0);
  assert(std::abs(vhb::headroom_percent(Sample{200, 150}) - 25.0) < 0.001);
  assert(vhb::headroom_percent(Sample{0, 0}) == 0.0);

  auto state =
      vhb::next_pressure_state(PressureState::healthy, Sample{100, 91}, 10, 15);
  assert(state == PressureState::pressured);
  state = vhb::next_pressure_state(state, Sample{100, 86}, 10, 15);
  assert(state == PressureState::pressured);
  state = vhb::next_pressure_state(state, Sample{100, 84}, 10, 15);
  assert(state == PressureState::healthy);
}
