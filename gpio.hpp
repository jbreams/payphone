
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <linux/gpio.h>

struct GpioFdHolder {
  int fd = -1;

  GpioFdHolder() = default;
  explicit GpioFdHolder(int fd) : fd(fd) {}

  GpioFdHolder(GpioFdHolder &&other) : fd(other.fd) {
    other.fd = -1;
  }

  GpioFdHolder &operator=(GpioFdHolder &&other) {
    close();
    fd = other.fd;
    other.fd = -1;
    return *this;
  }

  ~GpioFdHolder() { close(); }

  void close();
  operator int() const noexcept { return fd; }
};

struct GpioLineEventData {
  enum class EventId : uint32_t {
    RisingEdge = GPIO_V2_LINE_EVENT_RISING_EDGE,
    FallingEdge = GPIO_V2_LINE_EVENT_FALLING_EDGE
  };
  uint64_t timestamp_ns;
  EventId event_id;
  uint32_t idx;
  uint32_t seqno;
  uint32_t line_seqno;
};

struct GpioLineValues {
  struct AllOn {};

  GpioLineValues() = default;
  explicit GpioLineValues(uint64_t values) : values(values) {}
  explicit GpioLineValues(AllOn)
      : values(std::numeric_limits<uint64_t>::max()) {}

  GpioLineValues(std::initializer_list<uint32_t> value_indexes) {
    for (auto &&idx : value_indexes) {
      set(idx);
    }
  }

  friend bool operator==(const GpioLineValues &lhs, const GpioLineValues &rhs) {
    return lhs.values == rhs.values;
  }
  friend bool operator!=(const GpioLineValues &lhs, const GpioLineValues &rhs) {
    return lhs.values != rhs.values;
  }
  uint64_t values = 0;
  bool test(int idx) const noexcept { return (values >> idx & 0x1); }

  void set(int idx, bool on = true) noexcept {
    uint64_t mask = 1 << idx;
    if (on) {
      values |= mask;
    } else {
      values &= ~mask;
    }
  }
};

struct GpioLineFlags {
  constexpr static uint64_t Used = GPIO_V2_LINE_FLAG_USED;
  constexpr static uint64_t ActiveLow = GPIO_V2_LINE_FLAG_ACTIVE_LOW;
  constexpr static uint64_t Input = GPIO_V2_LINE_FLAG_INPUT;
  constexpr static uint64_t Output = GPIO_V2_LINE_FLAG_OUTPUT;
  constexpr static uint64_t EdgeRising = GPIO_V2_LINE_FLAG_EDGE_RISING;
  constexpr static uint64_t EdgeFalling = GPIO_V2_LINE_FLAG_EDGE_FALLING;
  constexpr static uint64_t OpenDrain = GPIO_V2_LINE_FLAG_OPEN_DRAIN;
  constexpr static uint64_t OpenSource = GPIO_V2_LINE_FLAG_OPEN_SOURCE;
  constexpr static uint64_t BiasPullUp = GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
  constexpr static uint64_t BiasPullDown = GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
  constexpr static uint64_t BiasDisabled = GPIO_V2_LINE_FLAG_BIAS_DISABLED;
  constexpr static uint64_t EventClockRealtime =
      GPIO_V2_LINE_FLAG_EVENT_CLOCK_REALTIME;
  constexpr static uint64_t EventClockHTE = GPIO_V2_LINE_FLAG_EVENT_CLOCK_HTE;
  uint64_t flags = 0;
};

struct GpioDebouncePeriod {
  std::chrono::microseconds period;
};

struct GpioLineAttribute {
  using AttrValue =
      std::variant<GpioLineFlags, GpioLineValues, GpioDebouncePeriod>;
  template <typename T,
            std::enable_if_t<std::is_constructible_v<AttrValue, T>, int> = 0>
  GpioLineAttribute(GpioLineValues idx, T value)
      : mask{idx}, attr(std::move(value)) {}

  GpioLineValues mask;
  AttrValue attr;
};

class GpioChip {
public:
  explicit GpioChip(const std::filesystem::path &path);
  ~GpioChip();

  struct LineConfig {
    GpioLineFlags flags;
    std::vector<GpioLineAttribute> attrs;
  };

  struct LineInfo {
    uint32_t idx;
    std::string name;
    std::string consumer;
    GpioLineFlags flags;
    std::vector<GpioLineAttribute> attrs;
  };

  class LineEventSource {
  public:
    LineEventSource() = default;
    int fd() const noexcept { return m_fd.fd; }
    std::vector<GpioLineEventData> read_events();

    void update_line_config(LineConfig &&config);
    GpioLineValues get_values(GpioLineValues mask);
    void set_values(GpioLineValues values, GpioLineValues mask);

  protected:
    friend class GpioChip;
    explicit LineEventSource(GpioFdHolder fd, size_t buffer_size)
        : m_fd(std::move(fd)), m_buffer_size(buffer_size) {}

  private:
    template <typename... Args> int do_ioctl(int ctl, Args... args);

    GpioFdHolder m_fd;
    size_t m_buffer_size;
  };

  const std::string &name() const noexcept { return m_name; }

  const std::string &label() const noexcept { return m_label; }

  size_t size() const noexcept { return m_lines; }

  LineInfo get_line_info(uint32_t idx, bool add_watch);
  void unwatch_line(uint32_t idx);

  LineEventSource make_line_event_source(std::vector<uint32_t> line_idxs,
                                         std::string consumer,
                                         LineConfig config,
                                         uint32_t event_buffer_size = 0);

protected:
  static void line_config_to_ioctl(const LineConfig &in,
                                   gpio_v2_line_config *out);

private:
  template <typename... Args> int do_ioctl(int ctl, Args... args);

  GpioFdHolder m_fd;
  std::string m_name;
  std::string m_label;
  uint32_t m_lines = 0;
  uint64_t m_all_lines_mask = 0;
};
