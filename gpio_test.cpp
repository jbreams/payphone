#include "dialer.hpp"
#include "gpio.hpp"

#include <array>
#include <iostream>
#include <thread>

#include <poll.h>

template <class... Ts> struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;
class GpioDialer : public Dialer {
public:
  GpioDialer(const std::filesystem::path &gpiochip,
             const std::array<std::string, 3> &columns,
             const std::array<std::string, 4> &rows)
      : m_chip(gpiochip) {
    std::array<uint32_t, 3> col_idxs;
    std::array<uint32_t, 4> row_idxs;
    size_t total_found = 0;
    size_t total_to_find = col_idxs.size() + row_idxs.size();

    for (uint32_t idx = 0; idx < m_chip.size() && total_found < total_to_find;
         ++idx) {
      auto pin = m_chip.get_line_info(idx, false);
      bool found_match = false;
      for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i] == pin.name) {
          col_idxs[i] = idx;
          ++total_found;
          found_match = true;
          break;
        }
      }

      if (found_match) {
        continue;
      }

      for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i] == pin.name) {
          row_idxs[i] = idx;
          ++total_found;
          break;
        }
      }
    }
    if (total_found != total_to_find) {
      throw std::runtime_error("Could not find all rows/columns in gpiochip");
    }

    std::vector<uint32_t> selectors = {row_idxs[0], row_idxs[1], row_idxs[2],
                                       row_idxs[3], col_idxs[0], col_idxs[1],
                                       col_idxs[2]};

    GpioChip::LineConfig line_config;
    GpioLineFlags row_flags{GpioLineFlags::Input | GpioLineFlags::BiasPullDown};
    GpioLineFlags column_flags{GpioLineFlags::Output | GpioLineFlags::BiasPullUp};

    line_config.attrs = {
        {{0, 1, 2, 3}, row_flags},
        {{0, 1, 2, 3}, GpioDebouncePeriod{std::chrono::milliseconds{1}}},
        {{4, 5, 6}, column_flags},
        {{4, 5, 6}, GpioLineValues{4, 5, 6}}};
    m_lines = m_chip.make_line_event_source(selectors, "PhoneDialer",
                                            std::move(line_config));
    for (auto &selector : selectors) {
      auto info = m_chip.get_line_info(selector, false);
      std::cout << "Line " << selector << " name: " << info.name
                << " consumer: " << info.consumer
                << " flags: " << info.flags.flags;
      for (auto &attr : info.attrs) {
        std::visit(overloaded{[&](const GpioDebouncePeriod &period) {
                                std::cout << " debounce period: "
                                          << period.period.count();
                              },
                              [&](const GpioLineValues &values) {
                                std::cout << " line values: " << values.values;
                              },
                              [&](const GpioLineFlags &flags) {
                                std::cout << " flags: " << flags.flags;
                              }},
                   attr.attr);
      }

      std::cout << std::endl;
    }
  }

  void interrupt() override {}
  EventData wait_for_event(std::optional<std::chrono::microseconds>) override {
    for (;;) {
      switch (m_state) {
      case State::Idle:
        if (auto cur_values = m_lines.get_values({0, 1, 2, 3});
            cur_values.values != 0) {
          m_state = State::Scanning;
          continue;
        }

        break;
      case State::Scanning:
        if (auto ch = scan_columns(); ch != '\0') {
          m_state = State::WaitForRelease;
          return EventData(ch);
        }
        break;
      case State::WaitForRelease:
        if (auto cur_values = m_lines.get_values({0, 1, 2, 3});
            cur_values.values == 0) {
          m_state = State::Idle;
          continue;
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return EventData(Event::WaitTimeout);
  }

private:
  enum class State { Idle, Scanning, WaitForRelease };

  char scan_columns() {
    constexpr static auto selectors =
        std::initializer_list<std::pair<uint32_t, std::array<char, 4>>>{
            {4, {'1', '4', '7', '*'}},
            {5, {'2', '5', '8', '0'}},
            {6, {'3', '6', '9', '#'}},
        };
    char found_ch = '\0';
    for (auto &col : selectors) {
      m_lines.set_values({col.first}, {4, 5, 6});

      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      auto values = m_lines.get_values({0, 1, 2, 3});
      for (size_t idx = 0; idx < col.second.size() && found_ch == '\0'; ++idx) {
        if (values.test(idx)) {
          found_ch = col.second[idx];
        }
      }
      if (found_ch != '\0') {
        break;
      }
    }

    m_lines.set_values({4, 5, 6}, {4, 5, 6});
    return found_ch;
  }

  State m_state = State::Idle;
  GpioChip m_chip;
  GpioChip::LineEventSource m_lines;
};

int main() {
  /*
BR RD OR YL GR BL PR
R1 c3 c2 c1 r4 r3 r2
20 13 21 26 19  6 5
  */

  std::array<std::string, 4> rows = {"GPIO20", "GPIO5", "GPIO6", "GPIO19"};
  std::array<std::string, 3> columns = {"GPIO26", "GPIO21", "GPIO13"};

  GpioDialer dialer("/dev/gpiochip0", columns, rows);

  for (;;) {
    auto event = dialer.wait_for_event(std::nullopt);
    std::cout << event.button << std::endl;
  }
  return 0;
}
