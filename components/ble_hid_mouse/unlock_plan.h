#pragma once
// Wake-and-unlock macro: pure step planner. No ESP-IDF or ESPHome dependencies, so
// tools/simulate_unlock.py compiles this exact header natively and checks the action stream.
//
// Sequence: small mouse wiggle (wakes the display), wait, Space (dismisses the screensaver / shows
// the login field), wait, Return, wait for the password field, type the password, optional Return.
// UNVERIFIED on hardware: the timings are guesses and US keyboard layout is assumed.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "hid_keys.h"

namespace esphome {
namespace ble_hid_mouse {

struct UnlockParams {
  uint16_t wake_dwell_ms{1500};   // after the wiggle, before Space
  uint16_t key_gap_ms{600};       // between Space and Return
  uint16_t field_dwell_ms{1000};  // after Return, before typing (password field appearing)
  uint16_t connect_wait_s{20};    // how long to wait for the Mac to (re)connect
  uint16_t cooldown_s{30};        // minimum time between live runs (avoids repeated wrong passwords)
  bool press_return{true};        // submit the password
  bool dry_run{false};            // wiggle only: send no key at all
  std::string password;
};

enum class UKind : uint8_t { MOVE, KEYDOWN, KEYUP, WAIT, DONE };

// step: 1 wiggle/wake, 2 Space, 3 Return, 4 password, 5 submit
struct UAction {
  UKind kind;
  int8_t dx, dy;      // MOVE
  uint8_t mod, usage; // KEYDOWN
  uint32_t delay_ms;  // wait after sending (WAIT: how long)
  uint8_t step;
};

class UnlockPlanner {
 public:
  // min_interval_ms: the negotiated BLE connection interval, rounded up. Every pacing interval is
  // raised to at least this so queued reports are not batched into one connection event.
  void start(const UnlockParams &p, uint32_t min_interval_ms) {
    this->q_.clear();
    this->idx_ = 0;
    this->down_ = false;
    this->cancelled_ = false;
    this->skipped_ = 0;
    this->typed_ = 0;
    this->pace_ = std::max<uint32_t>(30, min_interval_ms);
    const uint32_t move_ms = std::max<uint32_t>(40, min_interval_ms);

    // 1. A small square that returns to where it started (net zero), enough to wake the display.
    static const int8_t SQ[4][2] = {{12, 0}, {0, 12}, {-12, 0}, {0, -12}};
    for (const auto &m : SQ)
      this->q_.push_back(UAction{UKind::MOVE, m[0], m[1], 0, 0, move_ms, 1});
    if (p.dry_run) {
      this->q_.push_back(UAction{UKind::WAIT, 0, 0, 0, 0, p.wake_dwell_ms, 1});
      return;  // a dry run never sends a key
    }
    this->q_.push_back(UAction{UKind::WAIT, 0, 0, 0, 0, p.wake_dwell_ms, 1});
    // 2. Space, 3. Return
    this->tap(0, HID_KEY_SPACE, 2);
    this->q_.push_back(UAction{UKind::WAIT, 0, 0, 0, 0, p.key_gap_ms, 2});
    this->tap(0, HID_KEY_ENTER, 3);
    this->q_.push_back(UAction{UKind::WAIT, 0, 0, 0, 0, p.field_dwell_ms, 3});
    // 4. The password.
    for (char c : p.password) {
      uint8_t mod = 0, key = 0;
      if (ascii_to_hid(c, mod, key)) {
        this->tap(mod, key, 4);
        this->typed_++;
      } else {
        this->skipped_++;
      }
    }
    // 5. Submit.
    if (p.press_return) {
      this->q_.push_back(UAction{UKind::WAIT, 0, 0, 0, 0, 300, 5});
      this->tap(0, HID_KEY_ENTER, 5);
    }
  }

  // Abort at the next step boundary: release a held key, then finish.
  void cancel() { this->cancelled_ = true; }
  bool key_down() const { return this->down_; }
  uint32_t typed() const { return this->typed_; }
  uint32_t skipped() const { return this->skipped_; }
  // Wipe the queue: it holds the password as key codes.
  void clear() {
    this->q_.clear();
    this->idx_ = 0;
  }

  UAction next() {
    if (this->cancelled_) {
      if (this->down_) {
        this->down_ = false;
        return UAction{UKind::KEYUP, 0, 0, 0, 0, 0, this->step_};
      }
      this->q_.clear();
      this->idx_ = 0;
    }
    if (this->idx_ >= this->q_.size())
      return UAction{UKind::DONE, 0, 0, 0, 0, 0, this->step_};
    UAction a = this->q_[this->idx_++];
    this->step_ = a.step;
    if (a.kind == UKind::KEYDOWN)
      this->down_ = true;
    else if (a.kind == UKind::KEYUP)
      this->down_ = false;
    return a;
  }

 protected:
  void tap(uint8_t mod, uint8_t key, uint8_t step) {
    this->q_.push_back(UAction{UKind::KEYDOWN, 0, 0, mod, key, this->pace_, step});
    this->q_.push_back(UAction{UKind::KEYUP, 0, 0, 0, 0, this->pace_, step});
  }

  std::vector<UAction> q_;
  size_t idx_{0};
  bool down_{false};
  bool cancelled_{false};
  uint32_t pace_{30};
  uint32_t typed_{0};
  uint32_t skipped_{0};
  uint8_t step_{0};
};

}  // namespace ble_hid_mouse
}  // namespace esphome
