// Keyboard and consumer-key output, run on ESPHome's scheduler so nothing blocks. UNVERIFIED on
// hardware: whether macOS accepts the combo device, shows its Keyboard Setup Assistant, and which
// keys reach the lock screen have never been observed on a real Mac.
#include "ble_hid_mouse.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <cmath>

namespace esphome {
namespace ble_hid_mouse {

static const char *const TAG = "keyboard";

static constexpr uint32_t KEY_PACE_MIN_MS = 25;
static constexpr uint32_t KEY_PACE_UNKNOWN_MS = 40;
static constexpr size_t KEY_MAX_EVENTS = 200;

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

// Common start checks. Stops any jiggle in flight and undoes its displacement, like the sleep macro.
bool BLEHIDMouse::key_begin_(const char *what) {
  if (this->key_active_ || this->macro_active_) {
    this->key_status_ = "failed: busy";
    ESP_LOGW(TAG, "%s refused: another macro or key sequence is running", what);
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
  const uint16_t iv = this->macro_read_conn_();
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

bool BLEHIDMouse::consumer_tap(uint16_t usage) {
  if (!this->key_begin_("Consumer tap"))
    return false;
  if (!this->consumer_subscribed_) {
    this->key_status_ = "failed: consumer keys not subscribed (re-pair the Mac)";
    ESP_LOGW(TAG, "Consumer tap refused: %s", this->key_status_.c_str());
    return false;
  }
  this->key_queue_.assign(1, KeyEvt{0, 0, usage});
  this->key_idx_ = 0;
  this->key_down_ = false;
  this->key_active_ = true;
  this->key_status_ = "running";
  ESP_LOGI(TAG, "Consumer tap: usage=0x%04X (pace %u ms)", (unsigned) usage, (unsigned) this->key_pace_ms_);
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
    const bool ok = consumer ? this->send_consumer_report_(e.consumer) : this->send_key_report_(e.mod, e.usage);
    if (!ok) {
      this->key_finish_("failed: report not sent");
      return;
    }
    this->key_down_ = true;
  } else {
    const bool ok = consumer ? this->send_consumer_report_(0) : this->send_key_report_(0, 0);
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
  this->key_active_ = false;
  this->key_queue_.clear();  // may hold a password: do not keep it around
  this->key_idx_ = 0;
  this->key_status_ = status;
  ESP_LOGI(TAG, "Key sequence finished: %s", status);
}

}  // namespace ble_hid_mouse
}  // namespace esphome

#endif  // USE_ESP32
