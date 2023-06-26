#include "gpio.hpp"

#include <algorithm>

#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>

template <class... Ts> struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

void GpioFdHolder::close() {
  if (fd != -1) {
    ::close(fd);
  }
}

GpioChip::GpioChip(const std::filesystem::path &path) {
  m_fd = GpioFdHolder(::open(path.c_str(), O_RDWR));
  if (m_fd == -1) {
    int err = errno;
    throw std::system_error(err, std::system_category());
  }
  gpiochip_info info = {};
  do_ioctl(GPIO_GET_CHIPINFO_IOCTL, &info);
  m_name = info.name;
  m_label = info.label;
  m_lines = info.lines;
  GpioLineValues mask_builder;
  for (auto idx = 0; idx < m_lines; ++idx) {
    mask_builder.set(idx);
  }
  m_all_lines_mask = mask_builder.values;
}

GpioChip::~GpioChip() { ::close(m_fd); }

template <typename... Args> int GpioChip::do_ioctl(int ctl, Args... args) {
  int rc = ::ioctl(m_fd, ctl, args...);
  if (rc == -1) {
    int err = errno;
    throw std::system_error(err, std::system_category());
  }
  return rc;
}

void GpioChip::line_config_to_ioctl(const LineConfig &config,
                                    gpio_v2_line_config *ioctl_config) {
  ioctl_config->flags = config.flags.flags;
  ioctl_config->num_attrs = std::min(
      static_cast<size_t>(GPIO_V2_LINE_NUM_ATTRS_MAX), config.attrs.size());
  for (size_t idx = 0; idx < ioctl_config->num_attrs; ++idx) {
    auto &attr = config.attrs.at(idx);
    ioctl_config->attrs[idx].mask = attr.mask.values;
    std::visit(overloaded{[&](const GpioDebouncePeriod &period) {
                            ioctl_config->attrs[idx].attr.id =
                                GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
                            ioctl_config->attrs[idx].attr.debounce_period_us =
                                period.period.count();
                          },
                          [&](const GpioLineValues &values) {
                            ioctl_config->attrs[idx].attr.id =
                                GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
                            ioctl_config->attrs[idx].attr.values =
                                values.values;
                          },
                          [&](const GpioLineFlags &flags) {
                            ioctl_config->attrs[idx].attr.id =
                                GPIO_V2_LINE_ATTR_ID_FLAGS;
                            ioctl_config->attrs[idx].attr.flags = flags.flags;
                          }},
               attr.attr);
  }
}

void GpioChip::update_line_config(LineConfig &&config) {
  gpio_v2_line_config ioctl_config = {};
  line_config_to_ioctl(config, &ioctl_config);
  do_ioctl(GPIO_V2_LINE_SET_CONFIG_IOCTL, &ioctl_config);
}

GpioLineValues GpioChip::get_line_values(std::optional<GpioLineValues> mask) {
  gpio_v2_line_values ioctl_values = {};
  ioctl_values.mask = mask.value_or(GpioLineValues(m_all_lines_mask)).values;
  do_ioctl(GPIO_V2_LINE_GET_VALUES_IOCTL, &ioctl_values);
  return GpioLineValues(ioctl_values.bits);
}

void GpioChip::set_line_values(GpioLineValues values,
                               std::optional<GpioLineValues> mask) {
  gpio_v2_line_values ioctl_values = {};
  ioctl_values.mask = mask.value_or(GpioLineValues(m_all_lines_mask)).values;
  ioctl_values.bits = values.values;
  do_ioctl(GPIO_V2_LINE_SET_VALUES_IOCTL, &ioctl_values);
}

GpioChip::LineInfo GpioChip::get_line_info(uint32_t idx, bool add_watch) {
  gpio_v2_line_info ioctl_info = {};
  ioctl_info.offset = idx;
  do_ioctl(add_watch ? GPIO_V2_GET_LINEINFO_WATCH_IOCTL
                     : GPIO_V2_GET_LINEINFO_IOCTL,
           &ioctl_info);

  LineInfo ret;
  ret.name = ioctl_info.name;
  ret.consumer = ioctl_info.consumer;
  ret.flags = {ioctl_info.flags};

  for (size_t attr_idx = 0; attr_idx < ioctl_info.num_attrs; ++attr_idx) {
    switch (ioctl_info.attrs[attr_idx].id) {
    case GPIO_V2_LINE_ATTR_ID_DEBOUNCE:
      ret.attrs.emplace_back(
          idx, GpioDebouncePeriod{std::chrono::microseconds{
                   ioctl_info.attrs[attr_idx].debounce_period_us}});
      break;
    case GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES:
      ret.attrs.emplace_back(idx,
                             GpioLineValues(ioctl_info.attrs[attr_idx].values));
      break;
    case GPIO_V2_LINE_ATTR_ID_FLAGS:
      ret.attrs.emplace_back(idx,
                             GpioLineFlags{ioctl_info.attrs[attr_idx].flags});
      break;
    }
  }

  return ret;
}

GpioChip::LineEventSource
GpioChip::make_line_event_source(std::vector<uint32_t> line_idxs,
                                 std::string consumer, LineConfig config,
                                 uint32_t event_buffer_size) {
  gpio_v2_line_request ioctl_req = {};
  ioctl_req.num_lines =
      std::min(static_cast<size_t>(GPIO_V2_LINES_MAX), line_idxs.size());
  for (size_t idx = 0; idx < ioctl_req.num_lines; ++idx) {
    ioctl_req.offsets[idx] = line_idxs[idx];
  }

  if (!consumer.empty()) {
    strncpy(ioctl_req.consumer, consumer.c_str(),
            std::min(static_cast<size_t>(GPIO_MAX_NAME_SIZE), consumer.size()));
  }

  line_config_to_ioctl(config, &ioctl_req.config);
  ioctl_req.event_buffer_size = event_buffer_size;
  do_ioctl(GPIO_V2_GET_LINE_IOCTL, &ioctl_req);
  return LineEventSource(GpioFdHolder(ioctl_req.fd),
                         ioctl_req.event_buffer_size);
}

std::vector<GpioLineEventData> GpioChip::LineEventSource::read_events() {
  std::vector<gpio_v2_line_event> raw_buf;
  raw_buf.resize(std::min(m_buffer_size, static_cast<size_t>(16)));

  auto read_res =
      ::read(fd(), raw_buf.data(), raw_buf.size() * sizeof(gpio_v2_line_event));
  if (read_res == -1) {
    int err = errno;
    throw std::system_error(err, std::system_category());
  } else if (read_res < sizeof(gpio_v2_line_event)) {
    throw std::system_error(EIO, std::system_category());
  }

  raw_buf.resize(read_res / sizeof(gpio_v2_line_event));

  std::vector<GpioLineEventData> out;
  std::transform(raw_buf.begin(), raw_buf.end(), std::back_inserter(out),
                 [](const gpio_v2_line_event &raw) -> GpioLineEventData {
                   return {raw.timestamp_ns,
                           static_cast<GpioLineEventData::EventId>(raw.id),
                           raw.offset, raw.seqno, raw.line_seqno};
                 });
  return out;
}
