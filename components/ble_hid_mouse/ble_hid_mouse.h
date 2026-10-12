#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/display/display.h"
#include "jiggler_ui.h"  // icon drawing used by the YAML display lambda
#include "sleep_macro_plan.h"

#ifdef USE_ESP32

#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>

#include <string>
#include <vector>

namespace esphome {
namespace ble_hid_mouse {

static constexpr float MIN_INTERVAL_S = 10.0f;
static constexpr float MAX_INTERVAL_S = 3600.0f;
static constexpr uint8_t MIN_AMPLITUDE = 1;
static constexpr uint8_t MAX_AMPLITUDE = 5;

enum class AdvMode : uint8_t { NONE = 0, PAIRING, RECONNECT_FAST, RECONNECT_SLOW };
enum class AdvState : uint8_t { IDLE = 0, CONFIG_ADV, CONFIG_SCAN, STARTING, RUNNING, STOPPING };
enum class JigglePhase : uint8_t { IDLE = 0, OUT, GAP, BACK };

class BLEHIDMouse : public Component {
 public:
  // ---- configuration (codegen) ----
  void set_parent(esp32_ble::ESP32BLE *parent) { this->parent_ = parent; }
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_manufacturer(const std::string &s) { this->manufacturer_ = s; }
  void set_model(const std::string &s) { this->model_ = s; }
  void set_pnp_id(uint8_t source, uint16_t vid, uint16_t pid, uint16_t version);
  void set_pairing_timeout_ms(uint32_t ms) { this->pairing_timeout_ms_ = ms; }
  void set_battery_level(uint8_t level) { this->battery_level_ = level; }
  void set_connected_sensor(binary_sensor::BinarySensor *s) { this->connected_sensor_ = s; }
  void set_link_sensor(binary_sensor::BinarySensor *s) { this->link_sensor_ = s; }
  void set_bonded_sensor(binary_sensor::BinarySensor *s) { this->bonded_sensor_ = s; }
  void set_pairing_sensor(binary_sensor::BinarySensor *s) { this->pairing_sensor_ = s; }
  void add_on_jiggle_callback(std::function<void()> &&cb) { this->jiggle_callbacks_.add(std::move(cb)); }

  // ---- Component ----
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BLUETOOTH - 1.0f; }

  // ---- esp32_ble fan-out handlers ----
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
  void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

  // ---- pairing / bonding ----
  void start_pairing();  // also ends quiet mode
  void stop_pairing();
  void forget_host();
  bool pairing_active() const { return this->pairing_active_; }
  bool has_bond() const { return this->has_bond_; }
  uint32_t pairing_remaining_s() const;

  // ---- link state ----
  /// Host is connected, encrypted and has subscribed to input reports (we may send).
  bool is_ready() const { return this->connected_ && this->subscribed_; }
  bool is_link_connected() const { return this->connected_; }
  uint32_t seconds_since_disconnect() const;

  // ---- jiggler ----
  void set_enabled(bool enabled);
  void set_min_interval_s(float s);
  void set_max_interval_s(float s);
  void set_amplitude(uint8_t px);
  bool is_enabled() const { return this->enabled_; }
  bool jiggle_now();
  /// Seconds until the next scheduled jiggle, or -1 when nothing is scheduled.
  int32_t next_jiggle_in_s() const;
  /// Fraction 0..1 of the current interval that has elapsed (0 when nothing scheduled).
  float interval_progress() const;
  uint32_t jiggle_count() const { return this->jiggle_count_; }
  uint32_t ms_since_jiggle() const;
  bool jiggling() const { return this->jiggle_phase_ != JigglePhase::IDLE; }

  // ---- quiet mode: no advertising, no reconnect, until the jiggler is enabled or pairing starts ----
  void set_quiet(bool q);
  bool quiet() const { return this->quiet_; }

  // ---- sleep macro (UNVERIFIED on hardware; see README) ----
  MacroParams &macro_params() { return this->macro_p_; }
  /// Start the macro or a calibration test. Returns false (and sets the status) if refused.
  bool macro_start(MacroMode mode);
  void macro_cancel();
  bool macro_running() const { return this->macro_active_; }
  uint8_t macro_stage() const { return this->macro_active_ ? this->macro_stage_ : 0; }
  const std::string &macro_status() const { return this->macro_status_; }
  /// Incremented every time a run ends (any outcome). Poll it to react to completion.
  uint32_t macro_run_seq() const { return this->macro_seq_; }
  /// The last finished run was a full, non-dry run that saw the Mac go to sleep.
  bool macro_last_slept() const { return this->macro_slept_; }
  /// The last finished run was a full, non-dry run (any outcome).
  bool macro_last_was_live_full() const { return this->macro_live_full_; }
  /// 0 none, 1 success, 2 failure: the OLED result icon, for a few seconds after a run.
  uint8_t macro_result_icon() const;
  void macro_ack() { this->macro_result_ = 0; }
  /// Negotiated connection interval in ms (0 = unknown).
  float conn_interval_ms() const { return this->conn_x125_ * 1.25f; }

 protected:
  // GATT table construction (sequential, one request per completion event).
  struct Step {
    enum Kind : uint8_t { SERVICE, CHAR, DESCR, START } kind;
    uint16_t uuid;
    uint16_t perm;
    uint8_t prop;
    const uint8_t *value;
    uint16_t len;
    uint16_t max_len;
    uint16_t *handle_out;
    uint16_t num_handles;
  };
  void build_steps_();
  void build_next_();
  bool build_failed_(const char *what, esp_gatt_status_t status);

  // Advertising state machine.
  AdvMode desired_adv_mode_() const;
  void begin_adv_config_(AdvMode mode);
  void adv_reset_();

  // Security / bonds.
  void refresh_bond_state_();
  void remove_all_bonds_();

  // Reports.
  bool send_report_(int8_t dx, int8_t dy, uint8_t buttons = 0);  // only the macro passes buttons

  // Jiggle engine.
  void schedule_next_();
  void on_timer_();
  void start_jiggle_();
  void jiggle_step_();
  void abort_jiggle_();
  void get_effective_range_(float &lo, float &hi) const;

  void update_sensors_();

  esp32_ble::ESP32BLE *parent_{nullptr};

  // Identity.
  std::string device_name_;
  std::string manufacturer_;
  std::string model_;
  uint8_t pnp_[7]{};
  uint8_t battery_level_{100};
  uint32_t pairing_timeout_ms_{120000};

  // GATT.
  static constexpr uint16_t APP_ID = 0x4A1C;
  esp_gatt_if_t gatts_if_{ESP_GATT_IF_NONE};
  bool register_requested_{false};
  bool app_registered_{false};
  bool gatt_ready_{false};
  std::vector<Step> steps_;
  size_t step_idx_{0};
  uint16_t svc_handle_{0};
  uint16_t input_handle_{0};
  uint16_t input_cccd_handle_{0};
  uint8_t input_init_[4]{};

  // Link.
  bool connected_{false};
  bool encrypted_{false};
  bool subscribed_{false};
  uint16_t conn_id_{0};
  esp_bd_addr_t peer_{};
  uint32_t disconnected_since_ms_{0};

  // Bonding / pairing.
  bool has_bond_{false};
  bool pairing_active_{false};
  uint32_t pairing_until_ms_{0};

  // Advertising.
  AdvState adv_state_{AdvState::IDLE};
  AdvMode adv_target_{AdvMode::NONE};
  uint32_t adv_state_since_ms_{0};
  uint32_t reconnect_fast_until_ms_{0};

  // Jiggler settings.
  bool enabled_{false};
  float min_s_{120.0f};
  float max_s_{180.0f};
  uint8_t amplitude_{2};

  // Jiggler runtime.
  uint32_t next_fire_ms_{0};
  uint32_t interval_start_ms_{0};
  bool timer_pending_{false};
  JigglePhase jiggle_phase_{JigglePhase::IDLE};
  int8_t jig_ux_{0};
  int8_t jig_uy_{0};
  uint8_t jig_amp_{0};
  uint8_t jig_remaining_{0};
  int16_t net_x_{0};
  int16_t net_y_{0};
  uint32_t jiggle_count_{0};
  uint32_t last_jiggle_ms_{0};
  bool ever_jiggled_{false};

  // Published-state cache.
  binary_sensor::BinarySensor *connected_sensor_{nullptr};
  binary_sensor::BinarySensor *link_sensor_{nullptr};
  binary_sensor::BinarySensor *bonded_sensor_{nullptr};
  binary_sensor::BinarySensor *pairing_sensor_{nullptr};
  bool pub_init_{false};
  bool pub_connected_{false};
  bool pub_link_{false};
  bool pub_bonded_{false};
  bool pub_pairing_{false};

  CallbackManager<void()> jiggle_callbacks_;

  // Quiet mode.
  bool quiet_{false};

  // Sleep macro (implemented in sleep_macro.cpp).
  enum class MacroPhase : uint8_t { IDLE = 0, PREP, RUN, CONFIRM };
  void macro_set_status_(const char *s);
  void macro_prep_();
  void macro_next_();
  void macro_confirm_poll_();
  void macro_finish_(bool ok, const char *status, bool live_full, bool slept = false);
  void macro_release_();
  void macro_request_conn_(bool fast);
  uint16_t macro_read_conn_();
  void macro_on_disconnect_();
  MacroParams macro_p_;
  MacroPlanner macro_plan_;
  MacroMode macro_mode_{MacroMode::FULL};
  MacroPhase macro_phase_{MacroPhase::IDLE};
  bool macro_active_{false};
  bool macro_button_down_{false};
  bool macro_slept_{false};
  bool macro_live_full_{false};
  uint8_t macro_stage_{0};
  uint8_t macro_result_{0};
  uint32_t macro_result_ms_{0};
  uint32_t macro_seq_{0};
  uint32_t macro_prep_ms_{0};
  uint32_t macro_confirm_ms_{0};
  uint32_t macro_confirm_timeout_ms_{0};
  uint32_t macro_last_full_end_ms_{0};
  bool macro_have_last_full_{false};
  uint16_t conn_x125_{0};
  std::string macro_status_{"idle"};
};

class JiggleTrigger : public Trigger<> {
 public:
  explicit JiggleTrigger(BLEHIDMouse *parent) {
    parent->add_on_jiggle_callback([this]() { this->trigger(); });
  }
};

}  // namespace ble_hid_mouse
}  // namespace esphome

#endif  // USE_ESP32
