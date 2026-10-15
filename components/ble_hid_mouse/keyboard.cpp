// Keyboard and consumer-key output, run on ESPHome's scheduler so nothing blocks. UNVERIFIED on
// hardware: whether macOS accepts the combo device, shows its Keyboard Setup Assistant, and which
// keys reach the lock screen have never been observed on a real Mac.
#include "ble_hid_mouse.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace ble_hid_mouse {

static const char *const TAG = "keyboard";

static constexpr uint32_t KEY_PACE_MIN_MS = 25;
static constexpr uint32_t KEY_PACE_UNKNOWN_MS = 40;
static constexpr size_t KEY_MAX_EVENTS = 200;

// Current connection interval in 1.25 ms units as the host stack sees it (0 = unknown). ESPHome's
// esp32_ble drops the payload of the update-complete event, so we ask the stack instead.
uint16_t BLEHIDMouse::read_conn_interval_() {
  esp_gap_conn_params_t cp{};
  if (!this->connected_ || esp_ble_get_current_conn_params(this->peer_, &cp) != ESP_OK)
    return 0;
  return cp.interval;
}

bool BLEHIDMouse::send_key_report_(uint8_t modifiers, uint8_t usage) {
  if (!this->keyboard_ready() || this->kbd_handle_ == 0)
    return false;
  uint8_t report[8] = {modifiers, 0x00, usage, 0, 0, 0, 0, 0};
  esp_err_t err =
      esp_ble_gatts_send_indicate(this->gatts_if_, this->conn_id_, this->kbd_handle_, sizeof(report), report, false);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "send key report failed: %d", err);
    return false;
  }
  return true;
}

bool BLEHIDMouse::send_consumer_report_(uint16_t usage) {
  if (!this->connected_ || !this->consumer_subscribed_ || this->consumer_handle_ == 0)
    return false;
  uint8_t report[2] = {(uint8_t) (usage & 0xFF), (uint8_t) (usage >> 8)};
  esp_err_t err = esp_ble_gatts_send_indicate(this->gatts_if_, this->conn_id_, this->consumer_handle_, sizeof(report),
                                              report, false);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "send consumer report failed: %d", err);
    return false;
  }
  return true;
}

// Common start checks. Stops any jiggle in flight and undoes its displacement, before a key sequence.
bool BLEHIDMouse::key_begin_(const char *what) {
  if (this->key_active_) {
    this->key_status_ = "failed: busy";
    ESP_LOGW(TAG, "%s refused: another key sequence is running", what);
    return false;
  }
  if (!this->keyboard_ready()) {
    this->key_status_ = this->connected_ ? "failed: keyboard not subscribed (re-pair the Mac)" : "failed: not connected";
    ESP_LOGW(TAG, "%s refused: %s", what, this->key_status_.c_str());
    return false;
  }
  this->abort_jiggle_();
  if (this->net_x_ != 0 || this->net_y_ != 0) {
    if (this->send_report_((int8_t) -this->net_x_, (int8_t) -this->net_y_))
      this->net_x_ = this->net_y_ = 0;
  }
  const uint16_t iv = this->read_conn_interval_();
  this->key_pace_ms_ = iv != 0 ? std::max<uint32_t>(KEY_PACE_MIN_MS, (uint32_t) std::ceil(iv * 1.25f))
                               : KEY_PACE_UNKNOWN_MS;
  return true;
}

bool BLEHIDMouse::key_tap(uint8_t modifiers, uint8_t usage) {
  if (!this->key_begin_("Key tap"))
    return false;
  this->key_queue_.assign(1, KeyEvt{modifiers, usage, 0});
  this->key_idx_ = 0;
  this->key_down_ = false;
  this->key_active_ = true;
  this->key_status_ = "running";
  ESP_LOGI(TAG, "Key tap: modifiers=0x%02X usage=0x%02X (pace %u ms)", (unsigned) modifiers, (unsigned) usage,
           (unsigned) this->key_pace_ms_);
  this->key_step_();
  return true;
}

bool BLEHIDMouse::consumer_tap(uint16_t usage, uint8_t modifiers) {
  if (!this->key_begin_("Consumer tap"))
    return false;
  if (!this->consumer_subscribed_) {
    this->key_status_ = "failed: consumer keys not subscribed (re-pair the Mac)";
    ESP_LOGW(TAG, "Consumer tap refused: %s", this->key_status_.c_str());
    return false;
  }
  this->key_queue_.assign(1, KeyEvt{modifiers, 0, usage});
  this->key_idx_ = 0;
  this->key_down_ = false;
  this->key_active_ = true;
  this->key_status_ = "running";
  ESP_LOGI(TAG, "Consumer tap: usage=0x%04X modifiers=0x%02X (pace %u ms)", (unsigned) usage, (unsigned) modifiers,
           (unsigned) this->key_pace_ms_);
  this->key_step_();
  return true;
}

bool BLEHIDMouse::key_type(const std::string &text, bool enter_after) {
  if (!this->key_begin_("Type"))
    return false;
  std::vector<KeyEvt> q;
  size_t skipped = 0;
  for (char c : text) {
    KeyEvt e{0, 0, 0};
    if (ascii_to_hid(c, e.mod, e.usage) && q.size() < KEY_MAX_EVENTS)
      q.push_back(e);
    else
      skipped++;
  }
  if (enter_after)
    q.push_back(KeyEvt{0, HID_KEY_ENTER, 0});
  if (q.empty()) {
    this->key_status_ = "failed: nothing to type";
    ESP_LOGW(TAG, "Type refused: nothing typeable");
    return false;
  }
  this->key_queue_ = std::move(q);
  this->key_idx_ = 0;
  this->key_down_ = false;
  this->key_active_ = true;
  this->key_status_ = "running";
  // The text itself is never logged: it may be a password.
  ESP_LOGI(TAG, "Typing %u keys%s (pace %u ms)%s", (unsigned) this->key_queue_.size(),
           enter_after ? " incl. Return" : "", (unsigned) this->key_pace_ms_,
           skipped ? ", skipped untypeable characters" : "");
  this->key_step_();
  return true;
}

void BLEHIDMouse::key_step_() {
  if (!this->key_active_)
    return;
  if (!this->keyboard_ready()) {
    this->key_finish_("failed: link lost");
    return;
  }
  if (this->key_idx_ >= this->key_queue_.size()) {
    this->key_finish_("done");
    return;
  }
  const KeyEvt &e = this->key_queue_[this->key_idx_];
  const bool consumer = e.consumer != 0;
  if (!this->key_down_) {
    // A consumer key with modifiers: hold the modifiers on the keyboard channel around it.
    const bool ok = consumer ? ((e.mod == 0 || this->send_key_report_(e.mod, 0)) && this->send_consumer_report_(e.consumer))
                             : this->send_key_report_(e.mod, e.usage);
    if (!ok) {
      if (consumer && e.mod != 0)
        this->send_key_report_(0, 0);
      this->key_finish_("failed: report not sent");
      return;
    }
    this->key_down_ = true;
  } else {
    bool ok = consumer ? this->send_consumer_report_(0) : this->send_key_report_(0, 0);
    if (consumer && e.mod != 0)
      ok = this->send_key_report_(0, 0) && ok;
    if (!ok) {
      this->key_finish_("failed: report not sent");
      return;
    }
    this->key_down_ = false;
    this->key_idx_++;
  }
  this->set_timeout("key", this->key_pace_ms_, [this]() { this->key_step_(); });
}

// Never leave a key held: if the link is still up, send the release.
void BLEHIDMouse::key_release_() {
  if (!this->key_down_)
    return;
  if (this->key_idx_ < this->key_queue_.size() && this->connected_) {
    const KeyEvt &e = this->key_queue_[this->key_idx_];
    for (int i = 0; i < 3; i++) {
      if (e.consumer != 0 ? this->send_consumer_report_(0) : this->send_key_report_(0, 0))
        break;
    }
    if (e.consumer != 0 && e.mod != 0)
      this->send_key_report_(0, 0);
  }
  this->key_down_ = false;
}

void BLEHIDMouse::key_cancel() {
  if (!this->key_active_)
    return;
  ESP_LOGW(TAG, "Key sequence cancelled");
  this->key_finish_("failed: cancelled");
}

void BLEHIDMouse::key_on_disconnect_() {
  if (!this->key_active_)
    return;
  this->key_down_ = false;
  this->key_finish_("failed: link lost");
}

void BLEHIDMouse::key_finish_(const char *status) {
  this->cancel_timeout("key");
  this->key_release_();
  const bool was_unlock = this->unlock_phase_ != UnlockPhase::IDLE;
  if (was_unlock) {
    this->unlock_release_();
    const bool live = !this->unlock_p_.dry_run;
    this->unlock_phase_ = UnlockPhase::IDLE;
    this->unlock_plan_.clear();  // holds the password as key codes
    if (live) {
      this->unlock_have_last_ = true;
      this->unlock_last_end_ms_ = millis();
    }
    this->key_last_live_unlock_ = live;
  } else {
    this->key_last_live_unlock_ = false;
  }
  this->key_last_ok_ = strncmp(status, "done", 4) == 0;
  this->key_seq_++;
  this->key_active_ = false;
  this->key_queue_.clear();  // may hold a password: do not keep it around
  this->key_idx_ = 0;
  this->key_status_ = status;
  ESP_LOGI(TAG, "Key sequence finished: %s", status);
}

// ---------------------------------------------------------------------------------------------
// Wake-and-unlock macro. Reuses key_active_ so the jiggler and the key tests are excluded
// while it runs. UNVERIFIED on hardware: whether a sleeping Mac reconnects to us, and
// whether Space then Return reach the lock screen.
// ---------------------------------------------------------------------------------------------
static const char *const UNLOCK_STEP_STATUS[] = {
    "running", "running: waking the Mac", "running: Space", "running: Return", "running: typing password",
    "running: submitting",
};

bool BLEHIDMouse::unlock_start() {
  const UnlockParams &p = this->unlock_p_;
  auto refuse = [this](const char *why) {
    this->key_status_ = why;
    this->key_last_ok_ = false;
    this->key_last_live_unlock_ = false;
    this->key_seq_++;
    ESP_LOGW(TAG, "Unlock refused: %s", why);
    return false;
  };
  if (this->key_active_)
    return refuse("failed: busy");
  if (!p.dry_run && p.password.empty())
    return refuse("failed: no password set");
  if (!p.dry_run && this->unlock_have_last_ &&
      (millis() - this->unlock_last_end_ms_) < (uint32_t) p.cooldown_s * 1000u)
    return refuse("failed: cooldown");
  if (!this->connected_ && !this->has_bond_)
    return refuse("failed: no paired Mac");

  this->abort_jiggle_();
  if (this->is_ready() && (this->net_x_ != 0 || this->net_y_ != 0)) {
    if (this->send_report_((int8_t) -this->net_x_, (int8_t) -this->net_y_))
      this->net_x_ = this->net_y_ = 0;
  }
  this->key_active_ = true;
  this->unlock_phase_ = UnlockPhase::CONNECT;
  this->unlock_down_ = false;
  this->unlock_started_ms_ = millis();
  this->key_status_ = "running: waiting for the Mac";
  // The password itself is never logged.
  ESP_LOGI(TAG, "Unlock start%s: %u characters, wait up to %us for the Mac. Timings are UNVERIFIED guesses.",
           p.dry_run ? " (DRY RUN: wiggle only, no keys)" : "", (unsigned) p.password.size(),
           (unsigned) p.connect_wait_s);
  this->unlock_connect_poll_();
  return true;
}

void BLEHIDMouse::unlock_connect_poll_() {
  if (!this->key_active_ || this->unlock_phase_ != UnlockPhase::CONNECT)
    return;
  if (this->is_ready() && this->keyboard_ready()) {
    const uint16_t iv = this->read_conn_interval_();
    const uint32_t min_ms = iv != 0 ? (uint32_t) std::ceil(iv * 1.25f) : 40u;
    this->unlock_plan_.start(this->unlock_p_, min_ms);
    if (this->unlock_plan_.skipped() > 0)
      ESP_LOGW(TAG, "Unlock: %u password characters cannot be typed on a US layout and were skipped",
               (unsigned) this->unlock_plan_.skipped());
    this->unlock_phase_ = UnlockPhase::RUN;
    this->unlock_next_();
    return;
  }
  if ((millis() - this->unlock_started_ms_) >= (uint32_t) this->unlock_p_.connect_wait_s * 1000u) {
    this->key_finish_("failed: Mac did not reconnect");
    return;
  }
  this->set_timeout("key", 250, [this]() { this->unlock_connect_poll_(); });
}

void BLEHIDMouse::unlock_next_() {
  if (!this->key_active_ || this->unlock_phase_ != UnlockPhase::RUN)
    return;
  if (!this->is_ready() || !this->keyboard_ready()) {
    this->key_finish_("failed: link lost");
    return;
  }
  const UAction a = this->unlock_plan_.next();
  if (a.step >= 1 && a.step <= 5)
    this->key_status_ = UNLOCK_STEP_STATUS[a.step];
  switch (a.kind) {
    case UKind::MOVE:
      if (!this->send_report_(a.dx, a.dy)) {
        this->key_finish_("failed: report not sent");
        return;
      }
      this->set_timeout("key", a.delay_ms, [this]() { this->unlock_next_(); });
      break;
    case UKind::KEYDOWN:
      if (!this->send_key_report_(a.mod, a.usage)) {
        this->key_finish_("failed: report not sent");
        return;
      }
      this->unlock_down_ = true;
      this->set_timeout("key", a.delay_ms, [this]() { this->unlock_next_(); });
      break;
    case UKind::KEYUP:
      if (!this->send_key_report_(0, 0)) {
        this->key_finish_("failed: report not sent");
        return;
      }
      this->unlock_down_ = false;
      this->set_timeout("key", a.delay_ms, [this]() { this->unlock_next_(); });
      break;
    case UKind::WAIT:
      this->set_timeout("key", a.delay_ms, [this]() { this->unlock_next_(); });
      break;
    case UKind::DONE:
      this->key_finish_(this->unlock_p_.dry_run ? "done (dry run)" : "done");
      break;
  }
}

void BLEHIDMouse::unlock_release_() {
  if (this->unlock_down_ && this->keyboard_ready()) {
    for (int i = 0; i < 3 && !this->send_key_report_(0, 0); i++) {
    }
  }
  this->unlock_down_ = false;
}

}  // namespace ble_hid_mouse
}  // namespace esphome

#endif  // USE_ESP32
