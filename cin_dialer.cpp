
#include "dialer.hpp"

#include <algorithm>
#include <iostream>

#include <unistd.h>

CinDialer::CinDialer() { ::pipe(m_interrupt_pipe); }

CinDialer::~CinDialer() {
  ::close(m_interrupt_pipe[1]);
  ::close(m_interrupt_pipe[0]);
}

void CinDialer::interrupt() {
  char ch = 'i';
  ::write(m_interrupt_pipe[1], &ch, 1);
}

Dialer::EventData
CinDialer::wait_for_event(std::optional<std::chrono::microseconds> timeout) {
  for (;;) {
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(m_interrupt_pipe[0], &fdset);
    FD_SET(fileno(stdin), &fdset);
    struct timeval timeout_val = {};
    if (timeout) {
      timeout_val.tv_sec =
          std::chrono::duration_cast<std::chrono::seconds>(*timeout).count();
      timeout_val.tv_usec =
          (*timeout - std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::seconds(timeout_val.tv_sec)))
              .count();
      std::cerr << "timeout val: " << timeout_val.tv_sec << " "
                << timeout_val.tv_usec << "\n";
    }
    if (::select(std::max(fileno(stdin), m_interrupt_pipe[0]) + 1, &fdset,
                 nullptr, nullptr, timeout ? &timeout_val : nullptr) == 0) {
      return EventData(Event::WaitTimeout);
    }
    char ch = '\0';
    if (FD_ISSET(m_interrupt_pipe[0], &fdset)) {
      ::read(m_interrupt_pipe[0], &ch, 1);
      return EventData(Event::Interrupted);
    } else if (!FD_ISSET(fileno(stdin), &fdset)) {
      continue;
    }

    ::read(fileno(stdin), &ch, 1);
    if (ch == 'o') {
      return EventData(Event::OffHook);
    }
    if (ch == 'h') {
      return EventData(Event::OnHook);
    }
    if (ch == 'l') {
      return EventData(Event::LoudButton);
    }
    if ((ch >= '0' && ch <= '9') || ch == '#' || ch == '*') {
      return EventData(ch);
    }
  }
  __builtin_unreachable();
}
