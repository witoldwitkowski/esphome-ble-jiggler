#pragma once
// Sleep macro: pure step planner. No ESP-IDF or ESPHome dependencies, so tools/simulate_macro.py
// compiles this exact header natively and checks the HID report stream it produces.
//
// The macro drives the pointer to a display corner, drifts to the Apple logo, clicks it, drifts
// down to "Sleep" and clicks. Every positional number is a tunable parameter; the presets below
// are UNVERIFIED starting points, not measurements.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome {
namespace ble_hid_mouse {

enum class MacroMode : uint8_t { FULL = 0, TEST1 = 1, TEST2 = 2, TEST3 = 3, TEST4 = 4 };

// Numbers a preset can load. Units: counts are raw HID mouse counts, ms/s are time.
struct MacroNumbers {
  uint16_t slam_count;        // reports per axis per pass
  uint16_t slam_cpr;          // counts per slam report (1-127)
  uint16_t slam_interval_ms;
  uint16_t settle_ms;
  uint8_t slam_passes;        // 1 = X then Y; 2 repeats X then Y once more
  uint8_t drift_cps;          // counts per drift step (1-3)
  uint16_t drift_interval_ms;
  uint16_t drift_scale_pct;   // multiplier on every drift distance
  int16_t logo_x;             // counts from the corner to the Apple logo
  int16_t logo_y;
  int16_t sleep_y;            // counts down from the logo position to the Sleep row
  int16_t sleep_x;            // small X adjustment on that drift
  uint16_t dwell_before_ms;   // pause before clicking the logo
  uint16_t click_hold_ms;
  uint16_t menu_dwell_ms;     // wait for the menu to open
  uint16_t hover_dwell_ms;    // pause on Sleep before clicking
};

// UNVERIFIED starting points. Nobody has measured these on a real Mac; calibrate (see README).
inline const MacroNumbers &macro_preset(int idx) {
  static const MacroNumbers P[] = {
      // slam: count cpr  ivl  settle passes | drift: cps ivl scale | logo x y | sleep y x | dwell hold menu hover
      /* macOS 26 Tahoe   */ {40, 127, 15, 500, 1, 2, 20, 100, 24, 14, 190, 0, 300, 80, 800, 400},
      /* macOS 15 Sequoia */ {40, 127, 15, 500, 1, 2, 20, 100, 22, 13, 182, 0, 300, 80, 600, 350},
      /* macOS 14 Sonoma  */ {40, 127, 15, 500, 1, 2, 20, 100, 22, 13, 182, 0, 300, 80, 500, 350},
  };
  return P[idx < 0 ? 0 : (idx > 2 ? 2 : idx)];
}
inline const char *macro_preset_name(int idx) {
  static const char *const N[] = {"macOS 26 Tahoe", "macOS 15 Sequoia", "macOS 14 Sonoma"};
  return N[idx < 0 ? 0 : (idx > 2 ? 2 : idx)];
}

struct MacroParams : MacroNumbers {
  uint16_t confirm_timeout_s{30};
  uint16_t cooldown_s{60};
  bool top_right{false};   // mirror X: the Apple menu is at the top-right (right-to-left languages)
  bool dry_run{false};     // never press a button
  bool disable_jiggler{true};
  bool quiet{true};
  MacroParams() : MacroNumbers(macro_preset(0)) {}
};

// Stage numbers (also what the OLED shows):
//   1 corner slam, 2 settle, 3 drift to Apple logo, 4 click logo + menu dwell,
//   5 drift to Sleep + hover, 6 click Sleep, 7 confirm.
enum class ActKind : uint8_t { REPORT, WAIT, CONFIRM, DONE };

struct MacroAction {
  ActKind kind;
  uint8_t buttons;     // REPORT: button bitmask (bit 0 = left)
  int8_t dx, dy;       // REPORT
  uint32_t delay_ms;   // REPORT: wait after sending. WAIT: how long. CONFIRM: timeout.
  uint8_t stage;       // 1..7
};

class MacroPlanner {
 public:
  // min_interval_ms: the negotiated BLE connection interval, rounded up. Every pacing interval
  // is raised to at least this so queued reports are not batched into one connection event.
  void start(const MacroParams &p, MacroMode mode, uint32_t min_interval_ms) {
    this->p_ = p;
    this->mode_ = mode;
    this->min_iv_ = min_interval_ms;
    this->phase_ = Phase::SLAM;
    this->idx_ = 0;
    this->sub_ = 0;
    this->down_ = false;
    this->cancelled_ = false;
    this->stage_ = 1;
    this->p_.slam_cpr = clampu(this->p_.slam_cpr, 1, 127);
    this->p_.drift_cps = (uint8_t) clampu(this->p_.drift_cps, 1, 3);
    this->p_.slam_passes = (uint8_t) clampu(this->p_.slam_passes, 1, 3);
    this->p_.slam_count = (uint16_t) clampu(this->p_.slam_count, 1, 1000);
  }

  // Abort at the next step boundary: release a held button, never click, then finish.
  void cancel() { this->cancelled_ = true; }
  bool button_down() const { return this->down_; }
  uint8_t stage() const { return this->stage_; }

  // Scaled drift distances in counts (X already mirrored for the top-right corner).
  int drift_logo_x() const { return this->mirror(scaled(this->p_.logo_x)); }
  int drift_logo_y() const { return scaled(this->p_.logo_y); }
  int drift_sleep_x() const { return this->mirror(scaled(this->p_.sleep_x)); }
  int drift_sleep_y() const { return scaled(this->p_.sleep_y); }

  MacroAction next() {
    if (this->cancelled_) {
      if (this->down_) {
        this->down_ = false;
        return this->report(0, 0, 0, 0);
      }
      this->phase_ = Phase::END;
    }
    switch (this->phase_) {
      case Phase::SLAM: {
        const uint32_t per_pass = 2u * this->p_.slam_count;
        const uint32_t total = per_pass * this->p_.slam_passes;
        if (this->idx_ >= total) {
          this->phase_ = Phase::SETTLE;
          return this->next();
        }
        const bool x_axis = (this->idx_ % per_pass) < this->p_.slam_count;
        this->idx_++;
        this->stage_ = 1;
        const int c = (int) this->p_.slam_cpr;
        // X first (towards the corner's side), then Y negative (towards the top edge).
        return x_axis ? this->report(0, this->p_.top_right ? c : -c, 0, this->eff(this->p_.slam_interval_ms))
                      : this->report(0, 0, -c, this->eff(this->p_.slam_interval_ms));
      }
      case Phase::SETTLE:
        this->stage_ = 2;
        this->phase_ = this->mode_ == MacroMode::TEST1 ? Phase::END : Phase::DRIFT1;
        this->rx_ = this->drift_logo_x();
        this->ry_ = this->drift_logo_y();
        return this->wait(this->p_.settle_ms);
      case Phase::DRIFT1:
        this->stage_ = 3;
        if (this->rx_ == 0 && this->ry_ == 0) {
          this->phase_ = this->mode_ == MacroMode::TEST2 ? Phase::END : Phase::PRECLICK1;
          return this->next();
        }
        return this->drift_step(this->p_.drift_interval_ms);
      case Phase::PRECLICK1:
        this->stage_ = 4;
        this->phase_ = Phase::PRESS1;
        return this->wait(this->p_.dwell_before_ms);
      case Phase::PRESS1:
        this->stage_ = 4;
        this->phase_ = Phase::RELEASE1;
        if (this->p_.dry_run) return this->wait(this->p_.click_hold_ms);
        this->down_ = true;
        return this->report(1, 0, 0, this->p_.click_hold_ms);
      case Phase::RELEASE1:
        this->stage_ = 4;
        this->phase_ = Phase::MENU;
        if (this->p_.dry_run) return this->wait(0);
        this->down_ = false;
        return this->report(0, 0, 0, 0);
      case Phase::MENU:
        this->stage_ = 4;
        this->phase_ = this->mode_ == MacroMode::TEST3 ? Phase::END : Phase::DRIFT2_INIT;
        return this->wait(this->p_.menu_dwell_ms);
      case Phase::DRIFT2_INIT:
        this->rx_ = this->drift_sleep_x();
        this->ry_ = this->drift_sleep_y();
        this->phase_ = Phase::DRIFT2;
        return this->next();
      case Phase::DRIFT2:
        this->stage_ = 5;
        if (this->rx_ == 0 && this->ry_ == 0) {
          this->phase_ = Phase::HOVER;
          return this->next();
        }
        return this->drift_step(this->p_.drift_interval_ms);
      case Phase::HOVER:
        this->stage_ = 5;
        this->phase_ = this->mode_ == MacroMode::TEST4 ? Phase::END : Phase::PRESS2;
        return this->wait(this->p_.hover_dwell_ms);
      case Phase::PRESS2:
        this->stage_ = 6;
        this->phase_ = Phase::RELEASE2;
        if (this->p_.dry_run) return this->wait(this->p_.click_hold_ms);
        this->down_ = true;
        return this->report(1, 0, 0, this->p_.click_hold_ms);
      case Phase::RELEASE2:
        this->stage_ = 6;
        this->phase_ = this->p_.dry_run ? Phase::END : Phase::CONFIRM;
        if (this->p_.dry_run) return this->wait(0);
        this->down_ = false;
        return this->report(0, 0, 0, 0);
      case Phase::CONFIRM:
        this->stage_ = 7;
        this->phase_ = Phase::END;
        return MacroAction{ActKind::CONFIRM, 0, 0, 0, (uint32_t) this->p_.confirm_timeout_s * 1000u, 7};
      case Phase::END:
      default:
        return MacroAction{ActKind::DONE, 0, 0, 0, 0, this->stage_};
    }
  }

 protected:
  enum class Phase : uint8_t {
    SLAM, SETTLE, DRIFT1, PRECLICK1, PRESS1, RELEASE1, MENU, DRIFT2_INIT, DRIFT2, HOVER, PRESS2, RELEASE2, CONFIRM, END
  };

  static uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
  int scaled(int counts) const { return (int) std::lround((double) counts * this->p_.drift_scale_pct / 100.0); }
  int mirror(int x) const { return this->p_.top_right ? -x : x; }
  uint32_t eff(uint32_t ms) const { return std::max<uint32_t>(ms, this->min_iv_); }

  MacroAction report(uint8_t b, int dx, int dy, uint32_t delay) {
    return MacroAction{ActKind::REPORT, b, (int8_t) dx, (int8_t) dy, delay, this->stage_};
  }
  MacroAction wait(uint32_t ms) { return MacroAction{ActKind::WAIT, 0, 0, 0, ms, this->stage_}; }

  MacroAction drift_step(uint32_t interval_ms) {
    const int cps = this->p_.drift_cps;
    const int dx = std::max(-cps, std::min(cps, this->rx_));
    const int dy = std::max(-cps, std::min(cps, this->ry_));
    this->rx_ -= dx;
    this->ry_ -= dy;
    return this->report(0, dx, dy, this->eff(interval_ms));
  }

  MacroParams p_{};
  MacroMode mode_{MacroMode::FULL};
  uint32_t min_iv_{0};
  Phase phase_{Phase::END};
  uint32_t idx_{0};
  uint32_t sub_{0};
  int rx_{0}, ry_{0};
  bool down_{false};
  bool cancelled_{false};
  uint8_t stage_{0};
};

}  // namespace ble_hid_mouse
}  // namespace esphome
