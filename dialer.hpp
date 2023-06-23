#pragma once

#include <optional>

class Dialer {
public:
  virtual ~Dialer() = default;

  enum class Event {
    OffHook,
    OnHook,
    ButtonDown,
    LoudButton,
    Interrupted,
    WaitTimeout
  };
  struct EventData {
    explicit EventData(Event event) : event(event) {}

    explicit EventData(char ch) : event(Event::ButtonDown), button(ch) {}

    Event event;
    char button = '\0';
  };

  virtual void interrupt() = 0;

  virtual EventData
  wait_for_event(std::optional<std::chrono::microseconds> timeout) = 0;
};

class CinDialer : public Dialer {
public:
  CinDialer();

  ~CinDialer();

  void interrupt() override;

  EventData
  wait_for_event(std::optional<std::chrono::microseconds> timeout) override;

private:
  int m_interrupt_pipe[2];
};
