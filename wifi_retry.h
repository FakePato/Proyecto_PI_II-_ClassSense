#pragma once
#include <stdint.h>

// A single connection attempt owns the radio until success, failure or timeout.
// A failed attempt stops the radio before the next one; it never calls reconnect.
class WifiRetry {
 public:
  enum class Action { None, Start, Stop };
  static constexpr uint32_t ATTEMPT_MS = 30000;
  void started(uint32_t now) { phase_=Phase::Connecting; since_=now; }
  Action tick(uint32_t now, bool hasIP, bool failed) {
    if (hasIP) { phase_=Phase::Online; failures_=0; return Action::None; }
    if (phase_==Phase::Idle) { started(now); return Action::Start; }
    if (phase_==Phase::Connecting && !failed && uint32_t(now-since_)<ATTEMPT_MS)
      return Action::None;
    if (phase_==Phase::Online || phase_==Phase::Connecting) {
      phase_=Phase::Waiting; since_=now;
      wait_=5000U << (failures_<4 ? failures_ : 4);
      if (wait_>60000) wait_=60000;
      if (failures_<4) ++failures_;
      return Action::Stop;
    }
    if (uint32_t(now-since_)<wait_) return Action::None;
    started(now); return Action::Start;
  }
  uint32_t waitMilliseconds() const { return wait_; }
 private:
  enum class Phase { Idle, Connecting, Waiting, Online };
  Phase phase_=Phase::Idle;
  uint32_t since_=0, wait_=5000;
  uint8_t failures_=0;
};
