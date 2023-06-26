#include "gpio.hpp"

#include <iostream>

int main() {
  GpioChip chip("/dev/gpiochip0");
  std::cout << "Chip: " << chip.name() << " Label: " << chip.label()
            << std::endl;
  for (uint32_t idx = 0; idx < chip.size(); ++idx) {
    auto info = chip.get_line_info(idx, false);
    std::cout << "Line " << idx << " name: " << info.name
              << " flags: " << info.flags.flags << std::endl;
  }

  return 0;
}
