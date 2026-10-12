#pragma once

#include "esphome/core/defines.h"

#ifdef USE_JIGGLER_PORTAL

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/ble_hid_mouse/ble_hid_mouse.h"

#include <string>

namespace esphome {
namespace jiggler_portal {

static constexpr size_t MAX_NETS = 5;
static constexpr uint8_t CUSTOM_PROFILE = 255;

/// One saved Wi-Fi network.
struct NetRec {
  char ssid[33];
  char psk[65];
  uint8_t hidden;
};

struct NetStore {
  uint8_t magic;
  uint8_t count;
  NetRec nets[MAX_NETS];
};

/// Everything the page can change, persisted as one blob.
struct Config {
  uint8_t magic;
  uint8_t profile;       // index into the profile table, or CUSTOM_PROFILE
  uint8_t amplitude;     // px
  uint8_t flags;         // bit0 enabled, bit1 display off
  float min_s;
  float max_s;
  uint16_t dim_s;        // seconds before the OLED dims, 0 = never
  uint16_t restart_days; // 0 = never
  char name[25];         // custom identity
  char mfr[31];
  char model[31];
};

class JigglerPortal : public Component, public AsyncWebHandler {
 public:
  JigglerPortal(web_server_base::WebServerBase *base, ble_hid_mouse::BLEHIDMouse *mouse)
      : base_(base), mouse_(mouse) {}

  // ---- codegen ----
  void set_page(const uint8_t *data, size_t len) {
    this->page_ = data;
    this->page_len_ = len;
  }
  void set_defaults(uint32_t min_s, uint32_t max_s, uint8_t amp, uint8_t profile) {
    this->def_min_ = (float) min_s;
    this->def_max_ = (float) max_s;
    this->def_amp_ = amp;
    this->def_profile_ = profile;
  }
  void set_weak_signal_recovery(bool v) { this->weak_recovery_ = v; }
  void set_firmware_version(const std::string &v) { this->fw_version_ = v; }

  // ---- Component ----
  void setup() override;
  void loop() override;
  void dump_config() override;
  // Must run before wifi (to hand it the saved networks) and before ble_hid_mouse (identity).
  float get_setup_priority() const override { return setup_priority::BLUETOOTH + 10.0f; }

  // ---- AsyncWebHandler ----
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

  // ---- used by the display / button lambdas ----
  /// The device is offering its own setup network.
  bool setup_ap_active() const;
  bool wifi_connected() const;
  /// 0..4 bars, 0 when not connected.
  uint8_t wifi_bars() const;
  bool info_screen_active() const { return (int32_t) (millis() - this->info_until_ms_) < 0; }
  const char *ip_text() const { return this->ip_text_; }
  const char *tag() const { return this->tag_; }
  bool display_wanted() const { return !(this->cfg_.flags & 2); }
  bool dim_wanted() const;
  void touch() { this->last_activity_ms_ = millis(); }
  void on_button_short();
  void on_button_pair();
  void on_button_forget();
  void on_button_wifi_reset();
  void set_ota_state(bool active, float pct) {
    this->ota_active_ = active;
    this->ota_pct_ = pct;
  }
  bool ota_active() const { return this->ota_active_; }
  float ota_pct() const { return this->ota_pct_; }

 protected:
  // storage
  void load_();
  void save_cfg_();
  void save_nets_();
  void apply_identity_();
  void apply_jiggler_();
  void apply_networks_();

  // actions requested by HTTP handlers (run on the main loop)
  void schedule_restart_(uint32_t ms = 1500);
  bool add_network_(const std::string &ssid, const std::string &psk, bool hidden, std::string &err);
  bool remove_network_(const std::string &ssid);

  // http
  void send_json_(AsyncWebServerRequest *request, const std::string &body, int code = 200);
  void send_ok_(AsyncWebServerRequest *request);
  void send_err_(AsyncWebServerRequest *request, const char *msg, int code = 400);
  void api_scan_(AsyncWebServerRequest *request);
  void api_profiles_(AsyncWebServerRequest *request);
  void handle_api_(AsyncWebServerRequest *request, const char *path);

  // state
  void refresh_snapshot_();
  void update_ip_text_();
  void weak_signal_watchdog_(uint32_t now);

  web_server_base::WebServerBase *base_;
  ble_hid_mouse::BLEHIDMouse *mouse_;
  const uint8_t *page_{nullptr};
  size_t page_len_{0};
  std::string fw_version_;
  float def_min_{120}, def_max_{180};
  uint8_t def_amp_{2}, def_profile_{0};
  bool weak_recovery_{true};

  ESPPreferenceObject cfg_pref_;
  ESPPreferenceObject nets_pref_;
  Config cfg_{};
  NetStore nets_{};

  Mutex lock_;
  std::string snapshot_;

  bool first_loop_done_{false};
  bool server_started_{false};
  bool pending_restart_{false};
  bool reload_after_connect_{false};
  bool was_connected_{false};
  uint32_t last_snapshot_ms_{0};
  uint32_t last_activity_ms_{0};
  uint32_t info_until_ms_{0};
  uint32_t trying_since_ms_{0};
  bool jiggle_suppress_{false};

  // weak-signal watchdog
  uint32_t weak_since_ms_{0};
  uint32_t last_recovery_ms_{0};

  bool ota_active_{false};
  float ota_pct_{0};

  char ip_text_[16]{};
  char tag_[8]{};
};

}  // namespace jiggler_portal
}  // namespace esphome

#endif  // USE_JIGGLER_PORTAL
