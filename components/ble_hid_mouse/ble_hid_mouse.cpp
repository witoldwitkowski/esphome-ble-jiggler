#include "ble_hid_mouse.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <esp_bt_defs.h>
#include <esp_bt_main.h>
#include <esp_gatt_common_api.h>
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace ble_hid_mouse {

static const char *const TAG = "ble_hid_mouse";

// Advertising intervals, in 0.625 ms units.
static constexpr uint16_t ADV_PAIRING_MIN = 0x0030;  // 30 ms
static constexpr uint16_t ADV_PAIRING_MAX = 0x0050;  // 50 ms
static constexpr uint16_t ADV_FAST_MIN = 0x0030;     // 30 ms
static constexpr uint16_t ADV_FAST_MAX = 0x0060;     // 60 ms
static constexpr uint16_t ADV_SLOW_MIN = 0x0320;     // 500 ms
static constexpr uint16_t ADV_SLOW_MAX = 0x04B0;     // 750 ms
static constexpr uint32_t RECONNECT_FAST_WINDOW_MS = 30000;
static constexpr uint32_t ADV_STUCK_TIMEOUT_MS = 3000;

// Delay between 1-px steps within one half of a jiggle, and gap bounds between the halves.
static constexpr uint32_t STEP_MS_MIN = 12;
static constexpr uint32_t STEP_MS_MAX = 24;
static constexpr uint32_t GAP_MS_MIN = 30;
static constexpr uint32_t GAP_MS_MAX = 120;

// ---------------------------------------------------------------------------------------------
// HID report map: three top-level collections told apart by Report ID (the ID travels in the Report
// Reference descriptor, not in the data, so every payload keeps its plain layout):
//   ID 1 mouse     4 bytes [buttons, dx, dy, wheel]
//   ID 2 keyboard  8 bytes [modifiers, reserved, key1..key6] in; 1 byte LEDs out
//   ID 3 consumer  2 bytes [usage lo, usage hi]
// ---------------------------------------------------------------------------------------------
static const uint8_t REPORT_MAP[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Buttons)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x03,        //     Usage Maximum (3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data, Var, Abs)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x03,        //     Input (Const) padding
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data, Var, Rel)
    0xC0,              //   End Collection
    0xC0,              // End Collection
    // ---- keyboard ----
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Var, Abs): modifier bits
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const): reserved byte
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data, Var, Abs): LED bits
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Const): LED padding
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data, Array): six key slots
    0xC0,              // End Collection
    // ---- consumer control ----
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x03,        //   Report ID (3)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (1023)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array): one usage at a time
    0xC0,              // End Collection
};
static const uint8_t HID_INFO[] = {0x11, 0x01, 0x00, 0x02};  // bcdHID 1.11, country 0, normally connectable
static const uint8_t PROTOCOL_MODE_REPORT[] = {0x01};
static const uint8_t CONTROL_POINT_INIT[] = {0x00};
static const uint8_t REPORT_REF_MOUSE_IN[] = {0x01, 0x01};     // report ID 1, type Input
static const uint8_t REPORT_REF_KBD_IN[] = {0x02, 0x01};       // report ID 2, type Input
static const uint8_t REPORT_REF_KBD_OUT[] = {0x02, 0x02};      // report ID 2, type Output
static const uint8_t REPORT_REF_CONSUMER_IN[] = {0x03, 0x01};  // report ID 3, type Input
static const uint8_t CCCD_INIT[] = {0x00, 0x00};

static constexpr uint16_t UUID_DIS = 0x180A;
static constexpr uint16_t UUID_BATTERY = 0x180F;
static constexpr uint16_t UUID_HID = 0x1812;

static constexpr uint16_t PERM_OPEN_R = ESP_GATT_PERM_READ;
static constexpr uint16_t PERM_ENC_R = ESP_GATT_PERM_READ_ENCRYPTED;
static constexpr uint16_t PERM_ENC_RW = ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED;

static inline uint32_t rand_range(uint32_t lo, uint32_t hi) {  // inclusive
  if (hi <= lo)
    return lo;
  return lo + (random_uint32() % (hi - lo + 1));
}

void BLEHIDMouse::set_pnp_id(uint8_t source, uint16_t vid, uint16_t pid, uint16_t version) {
  this->pnp_[0] = source;
  this->pnp_[1] = vid & 0xFF;
  this->pnp_[2] = vid >> 8;
  this->pnp_[3] = pid & 0xFF;
  this->pnp_[4] = pid >> 8;
  this->pnp_[5] = version & 0xFF;
  this->pnp_[6] = version >> 8;
}

// ---------------------------------------------------------------------------------------------
// Setup / config dump / loop
// ---------------------------------------------------------------------------------------------
void BLEHIDMouse::setup() {
  this->disconnected_since_ms_ = millis();
  this->reconnect_fast_until_ms_ = millis() + RECONNECT_FAST_WINDOW_MS;
  this->input_handle_ = 0;
  this->build_steps_();
}

void BLEHIDMouse::dump_config() {
  ESP_LOGCONFIG(TAG,
                "BLE HID Mouse:\n"
                "  Name: %s\n"
                "  Manufacturer: %s\n"
                "  Model: %s\n"
                "  PnP: source=%u VID=0x%04X PID=0x%04X ver=0x%04X\n"
                "  Pairing timeout: %us",
                this->device_name_.c_str(), this->manufacturer_.c_str(), this->model_.c_str(), this->pnp_[0],
                (unsigned) (this->pnp_[1] | (this->pnp_[2] << 8)), (unsigned) (this->pnp_[3] | (this->pnp_[4] << 8)),
                (unsigned) (this->pnp_[5] | (this->pnp_[6] << 8)), (unsigned) (this->pairing_timeout_ms_ / 1000));
}

void BLEHIDMouse::loop() {
  if (this->parent_ == nullptr || !this->parent_->is_active()) {
    return;
  }
  const uint32_t now = millis();

  if (!this->register_requested_) {
    this->register_requested_ = true;
    esp_err_t err = esp_ble_gatts_app_register(APP_ID);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ble_gatts_app_register failed: %d", err);
      this->mark_failed();
    }
    return;
  }

  // Pairing window expiry.
  if (this->pairing_active_ && (int32_t) (now - this->pairing_until_ms_) >= 0) {
    ESP_LOGI(TAG, "Pairing mode timed out");
    this->stop_pairing();
  }

  // Advertising state machine.
  if (this->gatt_ready_) {
    AdvMode desired = this->desired_adv_mode_();
    switch (this->adv_state_) {
      case AdvState::IDLE:
        if (desired != AdvMode::NONE && !this->connected_) {
          this->begin_adv_config_(desired);
        }
        break;
      case AdvState::RUNNING:
        if (desired != this->adv_target_) {
          this->adv_state_ = AdvState::STOPPING;
          this->adv_state_since_ms_ = now;
          esp_ble_gap_stop_advertising();
        }
        break;
      default:
        if (now - this->adv_state_since_ms_ > ADV_STUCK_TIMEOUT_MS) {
          ESP_LOGW(TAG, "Advertising state %u stuck; resetting", (unsigned) this->adv_state_);
          this->adv_reset_();
        }
        break;
    }
  }

  this->update_sensors_();
}

// ---------------------------------------------------------------------------------------------
// GATT table construction
// ---------------------------------------------------------------------------------------------
void BLEHIDMouse::build_steps_() {
  using K = Step::Kind;
  auto S = [](uint16_t uuid, uint16_t handles) { return Step{K::SERVICE, uuid, 0, 0, nullptr, 0, 0, nullptr, handles}; };
  auto C = [](uint16_t uuid, uint16_t perm, uint8_t prop, const uint8_t *v, uint16_t len, uint16_t max_len,
              uint16_t *h) { return Step{K::CHAR, uuid, perm, prop, v, len, max_len, h, 0}; };
  auto D = [](uint16_t uuid, uint16_t perm, const uint8_t *v, uint16_t len, uint16_t *h) {
    return Step{K::DESCR, uuid, perm, 0, v, len, len, h, 0};
  };
  auto E = []() { return Step{K::START, 0, 0, 0, nullptr, 0, 0, nullptr, 0}; };

  const uint8_t R = ESP_GATT_CHAR_PROP_BIT_READ;
  const uint8_t N = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
  const uint8_t WNR = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
  const uint8_t W = ESP_GATT_CHAR_PROP_BIT_WRITE;

  this->steps_.clear();
  // Device Information (open: lets the host identify us before encryption).
  this->steps_.push_back(S(UUID_DIS, 8));
  this->steps_.push_back(C(0x2A29, PERM_OPEN_R, R, (const uint8_t *) this->manufacturer_.data(),
                           this->manufacturer_.size(), this->manufacturer_.size(), nullptr));
  this->steps_.push_back(C(0x2A24, PERM_OPEN_R, R, (const uint8_t *) this->model_.data(), this->model_.size(),
                           this->model_.size(), nullptr));
  this->steps_.push_back(C(0x2A50, PERM_OPEN_R, R, this->pnp_, sizeof(this->pnp_), sizeof(this->pnp_), nullptr));
  this->steps_.push_back(E());
  // Battery (static level).
  this->steps_.push_back(S(UUID_BATTERY, 6));
  this->steps_.push_back(C(0x2A19, PERM_OPEN_R, R | N, &this->battery_level_, 1, 1, nullptr));
  this->steps_.push_back(D(0x2902, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, CCCD_INIT, 2, nullptr));
  this->steps_.push_back(E());
  // HID.
  this->steps_.push_back(S(UUID_HID, 40));
  this->steps_.push_back(C(0x2A4A, PERM_ENC_R, R, HID_INFO, sizeof(HID_INFO), sizeof(HID_INFO), nullptr));
  this->steps_.push_back(C(0x2A4B, PERM_ENC_R, R, REPORT_MAP, sizeof(REPORT_MAP), sizeof(REPORT_MAP), nullptr));
  this->steps_.push_back(C(0x2A4C, PERM_ENC_RW, WNR, CONTROL_POINT_INIT, 1, 1, nullptr));
  this->steps_.push_back(C(0x2A4E, PERM_ENC_RW, R | WNR, PROTOCOL_MODE_REPORT, 1, 1, nullptr));
  this->steps_.push_back(C(0x2A4D, PERM_ENC_R, R | N, this->input_init_, sizeof(this->input_init_),
                           sizeof(this->input_init_), &this->input_handle_));
  this->steps_.push_back(D(0x2908, PERM_ENC_R, REPORT_REF_MOUSE_IN, 2, nullptr));
  this->steps_.push_back(D(0x2902, PERM_ENC_RW, CCCD_INIT, 2, &this->input_cccd_handle_));
  // Keyboard input, keyboard LED output, consumer input.
  this->steps_.push_back(C(0x2A4D, PERM_ENC_R, R | N, this->kbd_init_, sizeof(this->kbd_init_),
                           sizeof(this->kbd_init_), &this->kbd_handle_));
  this->steps_.push_back(D(0x2908, PERM_ENC_R, REPORT_REF_KBD_IN, 2, nullptr));
  this->steps_.push_back(D(0x2902, PERM_ENC_RW, CCCD_INIT, 2, &this->kbd_cccd_handle_));
  this->steps_.push_back(C(0x2A4D, PERM_ENC_RW, R | W | WNR, this->kbd_led_, sizeof(this->kbd_led_),
                           sizeof(this->kbd_led_), &this->kbd_led_handle_));
  this->steps_.push_back(D(0x2908, PERM_ENC_R, REPORT_REF_KBD_OUT, 2, nullptr));
  this->steps_.push_back(C(0x2A4D, PERM_ENC_R, R | N, this->consumer_init_, sizeof(this->consumer_init_),
                           sizeof(this->consumer_init_), &this->consumer_handle_));
  this->steps_.push_back(D(0x2908, PERM_ENC_R, REPORT_REF_CONSUMER_IN, 2, nullptr));
  this->steps_.push_back(D(0x2902, PERM_ENC_RW, CCCD_INIT, 2, &this->consumer_cccd_handle_));
  this->steps_.push_back(E());
}

bool BLEHIDMouse::build_failed_(const char *what, esp_gatt_status_t status) {
  if (status == ESP_GATT_OK)
    return false;
  ESP_LOGE(TAG, "GATT build failed at step %u (%s): status=%d", (unsigned) this->step_idx_, what, (int) status);
  this->mark_failed();
  return true;
}

void BLEHIDMouse::build_next_() {
  if (this->step_idx_ >= this->steps_.size()) {
    this->gatt_ready_ = true;
    ESP_LOGI(TAG, "GATT services started (input report handle=%u, cccd=%u)", this->input_handle_,
             this->input_cccd_handle_);
    return;
  }
  const Step &s = this->steps_[this->step_idx_];
  esp_err_t err = ESP_OK;
  switch (s.kind) {
    case Step::SERVICE: {
      esp_gatt_srvc_id_t id{};
      id.is_primary = true;
      id.id.inst_id = 0;
      id.id.uuid.len = ESP_UUID_LEN_16;
      id.id.uuid.uuid.uuid16 = s.uuid;
      err = esp_ble_gatts_create_service(this->gatts_if_, &id, s.num_handles);
      break;
    }
    case Step::CHAR: {
      esp_bt_uuid_t uuid{};
      uuid.len = ESP_UUID_LEN_16;
      uuid.uuid.uuid16 = s.uuid;
      esp_attr_value_t val{};
      val.attr_max_len = s.max_len;
      val.attr_len = s.len;
      val.attr_value = const_cast<uint8_t *>(s.value);
      esp_attr_control_t ctrl{};
      ctrl.auto_rsp = ESP_GATT_AUTO_RSP;
      err = esp_ble_gatts_add_char(this->svc_handle_, &uuid, (esp_gatt_perm_t) s.perm,
                                   (esp_gatt_char_prop_t) s.prop, &val, &ctrl);
      break;
    }
    case Step::DESCR: {
      esp_bt_uuid_t uuid{};
      uuid.len = ESP_UUID_LEN_16;
      uuid.uuid.uuid16 = s.uuid;
      esp_attr_value_t val{};
      val.attr_max_len = s.max_len;
      val.attr_len = s.len;
      val.attr_value = const_cast<uint8_t *>(s.value);
      esp_attr_control_t ctrl{};
      ctrl.auto_rsp = ESP_GATT_AUTO_RSP;
      err = esp_ble_gatts_add_char_descr(this->svc_handle_, &uuid, (esp_gatt_perm_t) s.perm, &val, &ctrl);
      break;
    }
    case Step::START:
      err = esp_ble_gatts_start_service(this->svc_handle_);
      break;
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "GATT build request failed at step %u: %d", (unsigned) this->step_idx_, err);
    this->mark_failed();
  }
}

// ---------------------------------------------------------------------------------------------
// GATTS events
// ---------------------------------------------------------------------------------------------
void BLEHIDMouse::gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                      esp_ble_gatts_cb_param_t *param) {
  if (event == ESP_GATTS_REG_EVT) {
    if (param->reg.app_id != APP_ID)
      return;
    if (param->reg.status != ESP_GATT_OK) {
      ESP_LOGE(TAG, "GATTS app registration failed: %d", (int) param->reg.status);
      this->mark_failed();
      return;
    }
    this->gatts_if_ = gatts_if;
    this->app_registered_ = true;

    // Security: LE Secure Connections + bonding, Just Works (no IO), distribute enc+id keys.
    uint8_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    uint8_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys));
    esp_ble_gap_config_local_icon(0x03C0);  // 0x03C0: mouse + keyboard + consumer

    this->refresh_bond_state_();
    ESP_LOGI(TAG, "GATTS registered (if=%u), %s", gatts_if, this->has_bond_ ? "bond present" : "no bond");
    this->step_idx_ = 0;
    this->build_next_();
    return;
  }
  if (gatts_if != this->gatts_if_ && gatts_if != ESP_GATT_IF_NONE) {
    return;
  }

  switch (event) {
    case ESP_GATTS_CREATE_EVT:
      if (this->build_failed_("create service", param->create.status))
        return;
      this->svc_handle_ = param->create.service_handle;
      this->step_idx_++;
      this->build_next_();
      break;
    case ESP_GATTS_ADD_CHAR_EVT: {
      if (this->build_failed_("add char", param->add_char.status))
        return;
      uint16_t *out = this->steps_[this->step_idx_].handle_out;
      if (out != nullptr)
        *out = param->add_char.attr_handle;
      this->step_idx_++;
      this->build_next_();
      break;
    }
    case ESP_GATTS_ADD_CHAR_DESCR_EVT: {
      if (this->build_failed_("add descriptor", param->add_char_descr.status))
        return;
      uint16_t *out = this->steps_[this->step_idx_].handle_out;
      if (out != nullptr)
        *out = param->add_char_descr.attr_handle;
      this->step_idx_++;
      this->build_next_();
      break;
    }
    case ESP_GATTS_START_EVT:
      if (this->build_failed_("start service", param->start.status))
        return;
      this->step_idx_++;
      this->build_next_();
      break;

    case ESP_GATTS_CONNECT_EVT: {
      this->connected_ = true;
      this->encrypted_ = false;
      this->subscribed_ = false;
      this->kbd_subscribed_ = false;
      this->consumer_subscribed_ = false;
      this->conn_id_ = param->connect.conn_id;
      memcpy(this->peer_, param->connect.remote_bda, sizeof(esp_bd_addr_t));
      // The stack stops advertising on connect; forget our view of it.
      this->adv_state_ = AdvState::IDLE;
      this->adv_target_ = AdvMode::NONE;
      ESP_LOGI(TAG, "Host connected (conn_id=%u, %02X:%02X:%02X:%02X:%02X:%02X)", this->conn_id_, this->peer_[0],
               this->peer_[1], this->peer_[2], this->peer_[3], this->peer_[4], this->peer_[5]);
      // Modest connection parameters: plenty responsive for a jiggler, friendly to Wi-Fi coexistence.
      esp_ble_conn_update_params_t cp{};
      memcpy(cp.bda, this->peer_, sizeof(esp_bd_addr_t));
      cp.min_int = 24;  // 30 ms
      cp.max_int = 40;  // 50 ms
      cp.latency = 4;
      cp.timeout = 400;  // 4 s
      esp_ble_gap_update_conn_params(&cp);
      // Prompt the host to (re-)encrypt / pair. Only when pairing is currently permitted or a bond exists.
      if (this->pairing_active_ || this->has_bond_) {
        esp_ble_set_encryption(this->peer_, ESP_BLE_SEC_ENCRYPT_NO_MITM);
      }
      break;
    }
    case ESP_GATTS_DISCONNECT_EVT:
      ESP_LOGI(TAG, "Host disconnected (reason=0x%02X)", (unsigned) param->disconnect.reason);
      this->connected_ = false;
      this->encrypted_ = false;
      this->subscribed_ = false;
      this->disconnected_since_ms_ = millis();
      this->reconnect_fast_until_ms_ = millis() + RECONNECT_FAST_WINDOW_MS;
      this->adv_state_ = AdvState::IDLE;
      this->adv_target_ = AdvMode::NONE;
      this->kbd_subscribed_ = false;
      this->consumer_subscribed_ = false;
      this->abort_jiggle_();
      this->macro_on_disconnect_();
      this->key_on_disconnect_();
      break;

    case ESP_GATTS_WRITE_EVT:
      if (param->write.handle == this->kbd_cccd_handle_ && param->write.len == 2) {
        this->kbd_subscribed_ = (param->write.value[0] & 0x01) != 0;
        ESP_LOGI(TAG, "Host %s keyboard reports", this->kbd_subscribed_ ? "subscribed to" : "unsubscribed from");
      } else if (param->write.handle == this->consumer_cccd_handle_ && param->write.len == 2) {
        this->consumer_subscribed_ = (param->write.value[0] & 0x01) != 0;
        ESP_LOGI(TAG, "Host %s consumer reports", this->consumer_subscribed_ ? "subscribed to" : "unsubscribed from");
      } else if (param->write.handle == this->kbd_led_handle_ && param->write.len >= 1) {
        ESP_LOGD(TAG, "Keyboard LEDs set by host: 0x%02X", (unsigned) param->write.value[0]);
      } else if (param->write.handle == this->input_cccd_handle_ && param->write.len == 2) {
        bool sub = (param->write.value[0] & 0x01) != 0;
        if (sub != this->subscribed_) {
          this->subscribed_ = sub;
          ESP_LOGI(TAG, "Host %s input reports", sub ? "subscribed to" : "unsubscribed from");
          if (sub && (this->net_x_ != 0 || this->net_y_ != 0)) {
            // Undo any displacement left by a jiggle that was cut short by a disconnect.
            int8_t cx = (int8_t) -this->net_x_;
            int8_t cy = (int8_t) -this->net_y_;
            if (this->send_report_(cx, cy)) {
              this->net_x_ = this->net_y_ = 0;
            }
          }
        }
      }
      break;

    case ESP_GATTS_MTU_EVT:
    case ESP_GATTS_CONF_EVT:
    case ESP_GATTS_RESPONSE_EVT:
    case ESP_GATTS_READ_EVT:
    case ESP_GATTS_EXEC_WRITE_EVT:
    case ESP_GATTS_SET_ATTR_VAL_EVT:
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------------------------
// GAP events (advertising + security)
// ---------------------------------------------------------------------------------------------
void BLEHIDMouse::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
      if (this->adv_state_ != AdvState::CONFIG_ADV)
        break;
      if (param->adv_data_raw_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "adv data set failed: %d", (int) param->adv_data_raw_cmpl.status);
        this->adv_reset_();
        break;
      }
      this->adv_state_ = AdvState::CONFIG_SCAN;
      this->adv_state_since_ms_ = millis();
      {
        uint8_t sr[31];
        size_t n = 0;
        size_t name_len = std::min<size_t>(this->device_name_.size(), 29);
        sr[n++] = (uint8_t) (name_len + 1);
        sr[n++] = ESP_BLE_AD_TYPE_NAME_CMPL;
        memcpy(sr + n, this->device_name_.data(), name_len);
        n += name_len;
        esp_ble_gap_config_scan_rsp_data_raw(sr, n);
      }
      break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT: {
      if (this->adv_state_ != AdvState::CONFIG_SCAN)
        break;
      if (param->scan_rsp_data_raw_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "scan response set failed: %d", (int) param->scan_rsp_data_raw_cmpl.status);
        this->adv_reset_();
        break;
      }
      esp_ble_adv_params_t p{};
      switch (this->adv_target_) {
        case AdvMode::PAIRING:
          p.adv_int_min = ADV_PAIRING_MIN;
          p.adv_int_max = ADV_PAIRING_MAX;
          break;
        case AdvMode::RECONNECT_FAST:
          p.adv_int_min = ADV_FAST_MIN;
          p.adv_int_max = ADV_FAST_MAX;
          break;
        default:
          p.adv_int_min = ADV_SLOW_MIN;
          p.adv_int_max = ADV_SLOW_MAX;
          break;
      }
      p.adv_type = ADV_TYPE_IND;
      p.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
      p.channel_map = ADV_CHNL_ALL;
      p.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
      this->adv_state_ = AdvState::STARTING;
      this->adv_state_since_ms_ = millis();
      esp_ble_gap_start_advertising(&p);
      break;
    }

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
      if (this->adv_state_ != AdvState::STARTING)
        break;
      if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "advertising start failed: %d", (int) param->adv_start_cmpl.status);
        this->adv_reset_();
        break;
      }
      this->adv_state_ = AdvState::RUNNING;
      ESP_LOGD(TAG, "Advertising (%s)",
               this->adv_target_ == AdvMode::PAIRING          ? "pairing"
               : this->adv_target_ == AdvMode::RECONNECT_FAST ? "reconnect-fast"
                                                              : "reconnect-slow");
      break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
      if (this->adv_state_ == AdvState::STOPPING) {
        this->adv_state_ = AdvState::IDLE;
        this->adv_target_ = AdvMode::NONE;
      }
      break;

    case ESP_GAP_BLE_SEC_REQ_EVT: {
      // A host is asking to pair. Only allow it in pairing mode (single bonded host policy).
      uint8_t *addr = param->ble_security.ble_req.bd_addr;
      if (this->pairing_active_) {
        ESP_LOGI(TAG, "Accepting pairing request (pairing mode)");
        // Single bonded host: drop any previous bond before the new one is created.
        this->remove_all_bonds_();
        esp_ble_gap_security_rsp(addr, true);
      } else {
        ESP_LOGW(TAG, "Rejecting pairing request (not in pairing mode)");
        esp_ble_gap_security_rsp(addr, false);
      }
      break;
    }

    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      const auto &ac = param->ble_security.auth_cmpl;
      if (ac.success) {
        this->encrypted_ = true;
        this->refresh_bond_state_();
        ESP_LOGI(TAG, "Link encrypted (bonded=%s)", this->has_bond_ ? "yes" : "no");
        if (this->pairing_active_) {
          ESP_LOGI(TAG, "Pairing complete");
          this->stop_pairing();
        }
      } else {
        ESP_LOGW(TAG, "Authentication failed, reason=0x%02X", (unsigned) ac.fail_reason);
        this->refresh_bond_state_();
      }
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------------------------------------
// Advertising helpers
// ---------------------------------------------------------------------------------------------
AdvMode BLEHIDMouse::desired_adv_mode_() const {
  if (!this->gatt_ready_ || this->connected_)
    return AdvMode::NONE;
  if (this->quiet_)
    return AdvMode::NONE;  // after a confirmed Mac sleep: stay silent so we cannot wake it
  if (this->pairing_active_)
    return AdvMode::PAIRING;
  if (this->has_bond_) {
    return ((int32_t) (this->reconnect_fast_until_ms_ - millis()) > 0) ? AdvMode::RECONNECT_FAST
                                                                     : AdvMode::RECONNECT_SLOW;
  }
  return AdvMode::NONE;  // no bond and not pairing: stay quiet (not discoverable)
}

void BLEHIDMouse::begin_adv_config_(AdvMode mode) {
  this->adv_target_ = mode;
  this->adv_state_ = AdvState::CONFIG_ADV;
  this->adv_state_since_ms_ = millis();
  const uint8_t flags = (mode == AdvMode::PAIRING) ? 0x06 : 0x04;  // general discoverable only when pairing
  const uint8_t adv[] = {
      0x02, ESP_BLE_AD_TYPE_FLAG, flags,                                         // flags
      0x05, ESP_BLE_AD_TYPE_16SRV_CMPL, UUID_HID & 0xFF, UUID_HID >> 8, UUID_BATTERY & 0xFF,
      UUID_BATTERY >> 8,                                                         // 0x1812, 0x180F
      0x03, ESP_BLE_AD_TYPE_APPEARANCE, 0xC0, 0x03,                              // generic HID
  };
  esp_err_t err = esp_ble_gap_config_adv_data_raw(const_cast<uint8_t *>(adv), sizeof(adv));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "config_adv_data_raw failed: %d", err);
    this->adv_reset_();
  }
}

void BLEHIDMouse::adv_reset_() {
  esp_ble_gap_stop_advertising();
  this->adv_state_ = AdvState::IDLE;
  this->adv_target_ = AdvMode::NONE;
}

// ---------------------------------------------------------------------------------------------
// Bonds and pairing mode
// ---------------------------------------------------------------------------------------------
void BLEHIDMouse::refresh_bond_state_() { this->has_bond_ = esp_ble_get_bond_device_num() > 0; }

void BLEHIDMouse::remove_all_bonds_() {
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) {
    this->has_bond_ = false;
    return;
  }
  auto *list = (esp_ble_bond_dev_t *) malloc(sizeof(esp_ble_bond_dev_t) * n);
  if (list == nullptr) {
    ESP_LOGE(TAG, "Out of memory listing bonds");
    return;
  }
  if (esp_ble_get_bond_device_list(&n, list) == ESP_OK) {
    for (int i = 0; i < n; i++) {
      esp_ble_remove_bond_device(list[i].bd_addr);
    }
    ESP_LOGI(TAG, "Removed %d bond(s)", n);
  }
  free(list);
  this->refresh_bond_state_();
}

void BLEHIDMouse::start_pairing() {
  if (!this->gatt_ready_) {
    ESP_LOGW(TAG, "start_pairing ignored: GATT not ready");
    return;
  }
  this->quiet_ = false;
  this->pairing_active_ = true;
  this->pairing_until_ms_ = millis() + this->pairing_timeout_ms_;
  ESP_LOGI(TAG, "Pairing mode ON for %us", (unsigned) (this->pairing_timeout_ms_ / 1000));
  if (this->connected_ && !this->encrypted_) {
    esp_ble_set_encryption(this->peer_, ESP_BLE_SEC_ENCRYPT_NO_MITM);
  }
}

void BLEHIDMouse::stop_pairing() {
  if (this->pairing_active_) {
    ESP_LOGI(TAG, "Pairing mode OFF");
  }
  this->pairing_active_ = false;
  this->reconnect_fast_until_ms_ = millis() + RECONNECT_FAST_WINDOW_MS;
}

void BLEHIDMouse::forget_host() {
  ESP_LOGW(TAG, "Forgetting paired host");
  this->remove_all_bonds_();
  if (this->connected_) {
    esp_ble_gatts_close(this->gatts_if_, this->conn_id_);
  }
  this->start_pairing();
}

uint32_t BLEHIDMouse::pairing_remaining_s() const {
  if (!this->pairing_active_)
    return 0;
  int32_t rem = (int32_t) (this->pairing_until_ms_ - millis());
  return rem > 0 ? (uint32_t) (rem / 1000) : 0;
}

uint32_t BLEHIDMouse::seconds_since_disconnect() const {
  if (this->is_ready())
    return 0;
  return (millis() - this->disconnected_since_ms_) / 1000;
}

// ---------------------------------------------------------------------------------------------
// HID reports
// ---------------------------------------------------------------------------------------------
bool BLEHIDMouse::send_report_(int8_t dx, int8_t dy, uint8_t buttons) {
  if (!this->is_ready() || this->input_handle_ == 0) {
    return false;
  }
  uint8_t report[4] = {(uint8_t) (buttons & 0x07), (uint8_t) dx, (uint8_t) dy, 0x00};  // wheel always 0
  esp_err_t err =
      esp_ble_gatts_send_indicate(this->gatts_if_, this->conn_id_, this->input_handle_, sizeof(report), report, false);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "send report failed: %d", err);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Jiggler
// ---------------------------------------------------------------------------------------------
void BLEHIDMouse::get_effective_range_(float &lo, float &hi) const {
  lo = this->min_s_;
  hi = this->max_s_;
  if (hi < lo)
    hi = lo;  // clamp: max can never be below min
}

void BLEHIDMouse::set_min_interval_s(float s) {
  if (s < MIN_INTERVAL_S)
    s = MIN_INTERVAL_S;
  if (s > MAX_INTERVAL_S)
    s = MAX_INTERVAL_S;
  this->min_s_ = s;
  if (this->min_s_ > this->max_s_) {
    ESP_LOGW(TAG, "Min interval (%.0fs) is above max (%.0fs); using max = min", this->min_s_, this->max_s_);
  }
  if (this->enabled_ && this->timer_pending_) {
    float lo, hi;
    this->get_effective_range_(lo, hi);
    if ((int32_t) (this->next_fire_ms_ - millis()) > (int32_t) (hi * 1000.0f))
      this->schedule_next_();
  }
}

void BLEHIDMouse::set_max_interval_s(float s) {
  if (s < MIN_INTERVAL_S)
    s = MIN_INTERVAL_S;
  if (s > MAX_INTERVAL_S)
    s = MAX_INTERVAL_S;
  this->max_s_ = s;
  if (this->min_s_ > this->max_s_) {
    ESP_LOGW(TAG, "Max interval (%.0fs) is below min (%.0fs); using max = min", this->max_s_, this->min_s_);
  }
  if (this->enabled_ && this->timer_pending_) {
    float lo, hi;
    this->get_effective_range_(lo, hi);
    if ((int32_t) (this->next_fire_ms_ - millis()) > (int32_t) (hi * 1000.0f))
      this->schedule_next_();
  }
}

void BLEHIDMouse::set_amplitude(uint8_t px) {
  if (px < MIN_AMPLITUDE)
    px = MIN_AMPLITUDE;
  if (px > MAX_AMPLITUDE)
    px = MAX_AMPLITUDE;
  this->amplitude_ = px;
}

void BLEHIDMouse::set_enabled(bool enabled) {
  if (enabled == this->enabled_ && (!enabled || this->timer_pending_))
    return;
  this->enabled_ = enabled;
  if (enabled && this->quiet_) {
    ESP_LOGI(TAG, "Quiet mode ended (jiggler enabled)");
    this->quiet_ = false;
  }
  ESP_LOGI(TAG, "Jiggler %s", enabled ? "enabled" : "disabled");
  if (enabled) {
    this->schedule_next_();
  } else {
    this->cancel_timeout("jig_next");
    this->timer_pending_ = false;
    this->next_fire_ms_ = 0;
  }
}

void BLEHIDMouse::schedule_next_() {
  float lo, hi;
  this->get_effective_range_(lo, hi);
  uint32_t delay_ms = rand_range((uint32_t) (lo * 1000.0f), (uint32_t) (hi * 1000.0f));
  this->interval_start_ms_ = millis();
  this->next_fire_ms_ = this->interval_start_ms_ + delay_ms;
  this->timer_pending_ = true;
  ESP_LOGD(TAG, "Next jiggle in %us", (unsigned) (delay_ms / 1000));
  this->set_timeout("jig_next", delay_ms, [this]() { this->on_timer_(); });
}

void BLEHIDMouse::on_timer_() {
  this->timer_pending_ = false;
  if (!this->enabled_)
    return;
  if (this->macro_active_ || this->key_active_) {
    ESP_LOGD(TAG, "Jiggle skipped: macro or key sequence running");
  } else if (this->is_ready()) {
    this->start_jiggle_();
  } else {
    ESP_LOGD(TAG, "Jiggle skipped: host not connected/subscribed");
  }
  this->schedule_next_();
}

bool BLEHIDMouse::jiggle_now() {
  if (!this->is_ready() || this->jiggling() || this->macro_active_ || this->key_active_) {
    ESP_LOGW(TAG, "Jiggle now ignored (%s)",
             (this->macro_active_ || this->key_active_) ? "macro or key sequence running" : this->jiggling() ? "already jiggling" : "host not ready");
    return false;
  }
  this->start_jiggle_();
  return true;
}

void BLEHIDMouse::start_jiggle_() {
  if (this->jiggling())
    return;
  this->jig_amp_ = (uint8_t) rand_range(1, this->amplitude_);
  switch (random_uint32() % 4) {
    case 0:
      this->jig_ux_ = 1, this->jig_uy_ = 0;
      break;
    case 1:
      this->jig_ux_ = -1, this->jig_uy_ = 0;
      break;
    case 2:
      this->jig_ux_ = 0, this->jig_uy_ = 1;
      break;
    default:
      this->jig_ux_ = 0, this->jig_uy_ = -1;
      break;
  }
  this->jig_remaining_ = this->jig_amp_;
  this->jiggle_phase_ = JigglePhase::OUT;
  this->last_jiggle_ms_ = millis();
  this->ever_jiggled_ = true;
  ESP_LOGI(TAG, "Jiggle: %u px (%d,%d) and back", this->jig_amp_, this->jig_ux_, this->jig_uy_);
  this->jiggle_step_();
}

void BLEHIDMouse::jiggle_step_() {
  switch (this->jiggle_phase_) {
    case JigglePhase::OUT:
      if (!this->send_report_(this->jig_ux_, this->jig_uy_)) {
        this->abort_jiggle_();
        return;
      }
      this->net_x_ += this->jig_ux_;
      this->net_y_ += this->jig_uy_;
      if (--this->jig_remaining_ == 0) {
        this->jiggle_phase_ = JigglePhase::GAP;
        this->set_timeout("jig_step", rand_range(GAP_MS_MIN, GAP_MS_MAX), [this]() { this->jiggle_step_(); });
      } else {
        this->set_timeout("jig_step", rand_range(STEP_MS_MIN, STEP_MS_MAX), [this]() { this->jiggle_step_(); });
      }
      break;
    case JigglePhase::GAP:
      this->jiggle_phase_ = JigglePhase::BACK;
      this->jig_remaining_ = this->jig_amp_;
      this->jiggle_step_();
      break;
    case JigglePhase::BACK:
      if (!this->send_report_(-this->jig_ux_, -this->jig_uy_)) {
        this->abort_jiggle_();
        return;
      }
      this->net_x_ -= this->jig_ux_;
      this->net_y_ -= this->jig_uy_;
      if (--this->jig_remaining_ == 0) {
        this->jiggle_phase_ = JigglePhase::IDLE;
        this->jiggle_count_++;
        this->jiggle_callbacks_.call();
      } else {
        this->set_timeout("jig_step", rand_range(STEP_MS_MIN, STEP_MS_MAX), [this]() { this->jiggle_step_(); });
      }
      break;
    default:
      break;
  }
}

void BLEHIDMouse::abort_jiggle_() {
  if (this->jiggle_phase_ != JigglePhase::IDLE) {
    ESP_LOGW(TAG, "Jiggle aborted mid-way (net offset %d,%d px will be undone on reconnect)", this->net_x_,
             this->net_y_);
  }
  this->cancel_timeout("jig_step");
  this->jiggle_phase_ = JigglePhase::IDLE;
}

int32_t BLEHIDMouse::next_jiggle_in_s() const {
  if (!this->enabled_ || !this->timer_pending_)
    return -1;
  int32_t rem = (int32_t) (this->next_fire_ms_ - millis());
  return rem > 0 ? (rem + 999) / 1000 : 0;
}

float BLEHIDMouse::interval_progress() const {
  if (!this->enabled_ || !this->timer_pending_)
    return 0.0f;
  uint32_t total = this->next_fire_ms_ - this->interval_start_ms_;
  if (total == 0)
    return 1.0f;
  uint32_t elapsed = millis() - this->interval_start_ms_;
  float f = (float) elapsed / (float) total;
  return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

uint32_t BLEHIDMouse::ms_since_jiggle() const {
  if (!this->ever_jiggled_)
    return UINT32_MAX;
  return millis() - this->last_jiggle_ms_;
}

// ---------------------------------------------------------------------------------------------
// Published state
// ---------------------------------------------------------------------------------------------
void BLEHIDMouse::update_sensors_() {
  bool c = this->is_ready();
  bool l = this->connected_;
  bool b = this->has_bond_;
  bool p = this->pairing_active_;
  if (!this->pub_init_ || c != this->pub_connected_) {
    this->pub_connected_ = c;
    if (this->connected_sensor_ != nullptr)
      this->connected_sensor_->publish_state(c);
  }
  if (!this->pub_init_ || l != this->pub_link_) {
    this->pub_link_ = l;
    if (this->link_sensor_ != nullptr)
      this->link_sensor_->publish_state(l);
  }
  if (!this->pub_init_ || b != this->pub_bonded_) {
    this->pub_bonded_ = b;
    if (this->bonded_sensor_ != nullptr)
      this->bonded_sensor_->publish_state(b);
  }
  if (!this->pub_init_ || p != this->pub_pairing_) {
    this->pub_pairing_ = p;
    if (this->pairing_sensor_ != nullptr)
      this->pairing_sensor_->publish_state(p);
  }
  this->pub_init_ = true;
}

}  // namespace ble_hid_mouse
}  // namespace esphome

#endif  // USE_ESP32
