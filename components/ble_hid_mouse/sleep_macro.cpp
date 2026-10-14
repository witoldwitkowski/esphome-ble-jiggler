// Sleep macro glue: runs the pure planner (sleep_macro_plan.h) on ESPHome's scheduler, so nothing
// blocks. UNVERIFIED on hardware: pointer landing positions, Tahoe menu geometry, the negotiated
// connection interval and the quiet-mode wake behavior have never been observed on a real Mac.
#include "ble_hid_mouse.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace ble_hid_mouse {

static const char *const TAG = "sleep_macro";

static const char *const STAGE_STATUS[] = {
    "running: preparing",         "running: corner slam",     "running: settling",     "running: drifting to Apple logo",
    "running: clicking Apple logo", "running: drifting to Sleep", "running: clicking Sleep", "running: waiting for sleep",
};

static constexpr uint32_t PREP_TIMEOUT_MS = 2000;
static constexpr uint16_t FAST_INT_MAX_X125 = 12;  // 15 ms: "short enough"
static constexpr uint32_t RESULT_OK_MS = 5000;
static constexpr uint32_t RESULT_FAIL_MS = 10000;

void BLEHIDMouse::set_quiet(bool q) {
  if (q != this->quiet_)
    ESP_LOGI(TAG, "Quiet mode %s", q ? "ON (no advertising until enabled or pairing)" : "OFF");
  this->quiet_ = q;
}

uint8_t BLEHIDMouse::macro_result_icon() const {
  if (this->macro_result_ == 0)
    return 0;
  const uint32_t limit = this->macro_result_ == 1 ? RESULT_OK_MS : RESULT_FAIL_MS;
  return (millis() - this->macro_result_ms_) < limit ? this->macro_result_ : 0;
}

void BLEHIDMouse::macro_set_status_(const char *s) {
  if (this->macro_status_ != s)
    this->macro_status_ = s;
}

void BLEHIDMouse::macro_request_conn_(bool fast) {
  if (!this->connected_)
    return;
  esp_ble_conn_update_params_t cp{};
  memcpy(cp.bda, this->peer_, sizeof(esp_bd_addr_t));
  if (fast) {
    cp.min_int = 6;    // 7.5 ms
    cp.max_int = 12;   // 15 ms
    cp.latency = 0;
    cp.timeout = 400;  // 4 s
  } else {
    cp.min_int = 24;   // 30 ms  (same as the request made at connect time)
    cp.max_int = 40;   // 50 ms
    cp.latency = 4;
    cp.timeout = 400;
  }
  esp_err_t err = esp_ble_gap_update_conn_params(&cp);
  ESP_LOGI(TAG, "Requested %s connection parameters (%.1f-%.1f ms, latency %u): %s", fast ? "fast" : "normal",
           cp.min_int * 1.25f, cp.max_int * 1.25f, (unsigned) cp.latency, err == ESP_OK ? "sent" : "failed");
}

// Current connection interval in 1.25 ms units as the host stack sees it (0 = unknown). ESPHome's
// esp32_ble drops the payload of the update-complete event, so we poll this instead.
uint16_t BLEHIDMouse::macro_read_conn_() {
  esp_gap_conn_params_t cp{};
  if (!this->connected_ || esp_ble_get_current_conn_params(this->peer_, &cp) != ESP_OK)
    return 0;
  return cp.interval;
}

bool BLEHIDMouse::macro_start(MacroMode mode) {
  if (this->macro_active_ || this->key_active_) {
    ESP_LOGW(TAG, "Start ignored: a macro or key sequence is already running");
    return false;
  }
  const bool full = mode == MacroMode::FULL;
  if (!this->is_ready()) {
    this->macro_set_status_("failed: not connected");
    this->macro_result_ = 2;
    this->macro_result_ms_ = millis();
    ESP_LOGW(TAG, "Refused: Bluetooth not connected");
    return false;
  }
  if (full && this->macro_have_last_full_ &&
      (millis() - this->macro_last_full_end_ms_) < (uint32_t) this->macro_p_.cooldown_s * 1000u) {
    this->macro_set_status_("failed: cooldown");
    this->macro_result_ = 2;
    this->macro_result_ms_ = millis();
    ESP_LOGW(TAG, "Refused: within the %us cooldown of the last run", (unsigned) this->macro_p_.cooldown_s);
    return false;
  }

  // Mutual exclusion with the jiggler: stop any jiggle in flight and undo its displacement.
  this->abort_jiggle_();
  if (this->net_x_ != 0 || this->net_y_ != 0) {
    if (this->send_report_((int8_t) -this->net_x_, (int8_t) -this->net_y_))
      this->net_x_ = this->net_y_ = 0;
  }

  this->macro_mode_ = mode;
  this->macro_active_ = true;
  this->macro_phase_ = MacroPhase::PREP;
  this->macro_button_down_ = false;
  this->macro_stage_ = 1;
  this->macro_result_ = 0;
  this->macro_slept_ = false;
  this->macro_live_full_ = false;
  this->macro_set_status_(STAGE_STATUS[0]);
  ESP_LOGI(TAG,
           "Start %s%s, corner %s. Positional numbers are UNVERIFIED starting values unless you calibrated them. "
           "logo=(%d,%d) sleep=(%d,%d) scale=%u%%",
           full ? "FULL macro" : "calibration test", this->macro_p_.dry_run ? " (DRY RUN: no clicks)" : "",
           this->macro_p_.top_right ? "top-right" : "top-left", (int) this->macro_p_.logo_x, (int) this->macro_p_.logo_y,
           (int) this->macro_p_.sleep_x, (int) this->macro_p_.sleep_y, (unsigned) this->macro_p_.drift_scale_pct);

  this->macro_request_conn_(true);
  this->macro_prep_ms_ = millis();
  this->set_timeout("macro", 50, [this]() { this->macro_prep_(); });
  return true;
}

void BLEHIDMouse::macro_prep_() {
  if (!this->macro_active_ || this->macro_phase_ != MacroPhase::PREP)
    return;
  if (!this->is_ready()) {
    this->macro_finish_(false, "failed: link lost", false);
    return;
  }
  const uint16_t iv = this->macro_read_conn_();
  const bool fast = iv != 0 && iv <= FAST_INT_MAX_X125;
  if (!fast && (millis() - this->macro_prep_ms_) < PREP_TIMEOUT_MS) {
    this->set_timeout("macro", 50, [this]() { this->macro_prep_(); });
    return;
  }
  this->conn_x125_ = iv;
  // Unknown interval: assume the 30 ms we asked for at connect time.
  const uint32_t min_ms = iv != 0 ? (uint32_t) std::ceil(iv * 1.25f) : 30u;
  if (fast)
    ESP_LOGI(TAG, "Negotiated connection interval: %.2f ms", iv * 1.25f);
  else
    ESP_LOGW(TAG, "Short connection interval not granted (got %s%.2f ms); pacing every step at >= %u ms",
             iv ? "" : "unknown, assuming ", iv ? iv * 1.25f : 30.0f, (unsigned) min_ms);
  this->macro_plan_.start(this->macro_p_, this->macro_mode_, min_ms);
  this->macro_phase_ = MacroPhase::RUN;
  this->macro_next_();
}

void BLEHIDMouse::macro_next_() {
  if (!this->macro_active_ || this->macro_phase_ != MacroPhase::RUN)
    return;
  const bool full = this->macro_mode_ == MacroMode::FULL;
  const bool live = full && !this->macro_p_.dry_run;
  if (!this->is_ready()) {
    this->macro_finish_(false, "failed: link lost", live);
    return;
  }
  const MacroAction a = this->macro_plan_.next();
  if (a.stage >= 1 && a.stage <= 7) {
    this->macro_stage_ = a.stage;
    this->macro_set_status_(STAGE_STATUS[a.stage]);
  }
  switch (a.kind) {
    case ActKind::REPORT:
      if (!this->send_report_(a.dx, a.dy, a.buttons)) {
        this->macro_release_();
        this->macro_finish_(false, "failed: report not sent", live);
        return;
      }
      this->macro_button_down_ = (a.buttons & 1) != 0;
      this->set_timeout("macro", a.delay_ms, [this]() { this->macro_next_(); });
      break;
    case ActKind::WAIT:
      this->set_timeout("macro", a.delay_ms, [this]() { this->macro_next_(); });
      break;
    case ActKind::CONFIRM:
      this->macro_phase_ = MacroPhase::CONFIRM;
      this->macro_confirm_ms_ = millis();
      this->macro_confirm_timeout_ms_ = a.delay_ms;
      this->macro_confirm_poll_();
      break;
    case ActKind::DONE:
      if (full)
        this->macro_finish_(true, "succeeded (dry run)", false);
      else {
        char buf[24];
        snprintf(buf, sizeof(buf), "test %u done", (unsigned) this->macro_mode_);
        this->macro_finish_(true, buf, false);
      }
      break;
  }
}

void BLEHIDMouse::macro_confirm_poll_() {
  if (!this->macro_active_ || this->macro_phase_ != MacroPhase::CONFIRM)
    return;
  if (!this->is_ready()) {  // normally caught earlier by macro_on_disconnect_()
    this->macro_finish_(true, "succeeded", true, true);
    return;
  }
  if ((millis() - this->macro_confirm_ms_) >= this->macro_confirm_timeout_ms_) {
    this->macro_finish_(false, "failed: no sleep detected", true);
    return;
  }
  this->set_timeout("macro", 100, [this]() { this->macro_confirm_poll_(); });
}

// Releases the button even if the macro is cancelled or fails. If the link is already gone the
// host has dropped the button state with it.
void BLEHIDMouse::macro_release_() {
  if (this->macro_button_down_) {
    if (this->is_ready()) {
      for (int i = 0; i < 3 && !this->send_report_(0, 0, 0); i++) {
      }
    }
    this->macro_button_down_ = false;
  }
}

void BLEHIDMouse::macro_cancel() {
  if (!this->macro_active_)
    return;
  ESP_LOGW(TAG, "Cancelled (stage %u). Pointer parked; nothing clicked.", (unsigned) this->macro_stage_);
  this->cancel_timeout("macro");
  this->macro_plan_.cancel();
  this->macro_release_();
  this->macro_finish_(false, "failed: cancelled", false);
}

void BLEHIDMouse::macro_on_disconnect_() {
  if (!this->macro_active_)
    return;
  this->macro_button_down_ = false;
  this->cancel_timeout("macro");
  this->conn_x125_ = 0;
  if (this->macro_phase_ == MacroPhase::CONFIRM)
    this->macro_finish_(true, "succeeded", true, true);
  else
    this->macro_finish_(false, "failed: link lost", this->macro_mode_ == MacroMode::FULL && !this->macro_p_.dry_run);
}

void BLEHIDMouse::macro_finish_(bool ok, const char *status, bool live_full, bool slept) {
  this->cancel_timeout("macro");
  this->macro_release_();
  this->macro_active_ = false;
  this->macro_phase_ = MacroPhase::IDLE;
  this->macro_stage_ = 0;
  this->macro_set_status_(status);
  this->macro_result_ = ok ? 1 : 2;
  this->macro_result_ms_ = millis();
  this->macro_live_full_ = live_full;
  this->macro_slept_ = slept;
  if (live_full) {
    this->macro_have_last_full_ = true;
    this->macro_last_full_end_ms_ = millis();
  }
  this->macro_request_conn_(false);  // no-op if the link is gone
  if (slept && this->macro_p_.quiet)
    this->set_quiet(true);  // before the advertising state machine can react to the disconnect
  this->macro_seq_++;
  ESP_LOGI(TAG, "Finished: %s%s", status, slept ? " (Mac slept: link dropped)" : "");
}

}  // namespace ble_hid_mouse
}  // namespace esphome

#endif  // USE_ESP32
