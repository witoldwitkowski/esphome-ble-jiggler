#include "jiggler_portal.h"

#ifdef USE_JIGGLER_PORTAL

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/components/wifi/scan_list.h"

#include <esp_system.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace esphome {
namespace jiggler_portal {

static const char *const TAG = "jiggler_portal";

static constexpr uint8_t CFG_MAGIC = 0xA7;
static constexpr uint8_t NETS_MAGIC = 0xB3;
static constexpr uint32_t CFG_KEY = 0x4A494701;
static constexpr uint32_t NETS_KEY = 0x4A494702;

// How long the OLED shows the address after Wi-Fi comes up or the button is tapped.
static constexpr uint32_t INFO_AFTER_CONNECT_MS = 20000;
static constexpr uint32_t INFO_AFTER_TAP_MS = 8000;

// Weak-signal recovery: connected, but worse than this for this long, and not tried recently.
static constexpr int8_t WEAK_RSSI_DBM = -78;
static constexpr uint32_t WEAK_FOR_MS = 10UL * 60UL * 1000UL;
static constexpr uint32_t RECOVERY_COOLDOWN_MS = 30UL * 60UL * 1000UL;

// ---------------------------------------------------------------------------------------------
// Mouse identities. Vendor/product IDs for the Logitech entries come from public USB ID lists;
// the other brands use a generic product id. Hosts only use these as labels.
// ---------------------------------------------------------------------------------------------
struct Profile {
  const char *label;  // shown in the page
  const char *name;   // Bluetooth name (<= 24 chars)
  const char *mfr;
  const char *model;
  uint8_t src;  // 1 = Bluetooth SIG, 2 = USB-IF
  uint16_t vid, pid, ver;
};

static const Profile PROFILES[] = {
    {"Logitech MX Anywhere 3", "MX Anywhere 3", "Logitech", "MX Anywhere 3", 2, 0x046D, 0xB025, 0x0001},
    {"Logitech MX Master 3S", "MX Master 3S", "Logitech", "MX Master 3S", 2, 0x046D, 0xB034, 0x0001},
    {"Logitech MX Master 3", "MX Master 3", "Logitech", "MX Master 3", 2, 0x046D, 0xB023, 0x0001},
    {"Logitech MX Anywhere 2S", "MX Anywhere 2S", "Logitech", "MX Anywhere 2S", 2, 0x046D, 0xB01A, 0x0001},
    {"Logitech M720 Triathlon", "M720 Triathlon", "Logitech", "M720 Triathlon", 2, 0x046D, 0xB015, 0x0001},
    {"Microsoft Bluetooth Mouse", "Microsoft Mouse", "Microsoft", "Bluetooth Mouse", 2, 0x045E, 0x0001, 0x0001},
    {"Dell Wireless Mouse", "Dell Wireless Mouse", "Dell", "Wireless Mouse", 2, 0x413C, 0x0001, 0x0001},
    {"HP Bluetooth Mouse", "HP Bluetooth Mouse", "HP", "Bluetooth Mouse", 2, 0x03F0, 0x0001, 0x0001},
    {"Lenovo Bluetooth Mouse", "Lenovo BT Mouse", "Lenovo", "Bluetooth Mouse", 2, 0x17EF, 0x0001, 0x0001},
    {"Generic Bluetooth mouse", "Bluetooth Mouse", "Generic", "BT Mouse", 2, 0x1209, 0x0001, 0x0001},
};
static constexpr size_t N_PROFILES = sizeof(PROFILES) / sizeof(PROFILES[0]);

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------
static void jstr(std::string &o, const char *s) {
  o += '"';
  for (; *s; s++) {
    const unsigned char c = (unsigned char) *s;
    if (c == '"' || c == '\\') {
      o += '\\';
      o += (char) c;
    } else if (c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else {
      o += (char) c;
    }
  }
  o += '"';
}

static void jnum(std::string &o, long v) {
  char b[24];
  snprintf(b, sizeof(b), "%ld", v);
  o += b;
}

static void jfloat(std::string &o, float v, int dp) {
  char b[24];
  snprintf(b, sizeof(b), "%.*f", dp, (double) v);
  o += b;
}

static void copy_str(char *dst, size_t cap, const std::string &s) {
  memset(dst, 0, cap);
  strncpy(dst, s.c_str(), cap - 1);
}

static std::string trimmed(std::string s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
  return s.substr(a, b - a);
}

static bool printable(const std::string &s) {
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7F) return false;
  return true;
}

static uint8_t bars_from_rssi(int rssi) {
  if (rssi > -55) return 4;
  if (rssi > -65) return 3;
  if (rssi > -75) return 2;
  return 1;
}

// ---------------------------------------------------------------------------------------------
// Setup / storage
// ---------------------------------------------------------------------------------------------
void JigglerPortal::setup() {
  uint8_t mac[6];
  get_mac_address_raw(mac);
  snprintf(this->tag_, sizeof(this->tag_), "%02x%02x%02x", mac[3], mac[4], mac[5]);

  this->load_();
  this->apply_identity_();
  this->apply_jiggler_();
  this->apply_networks_();

  // Registered before the stock captive portal's handler (which registers when its access point
  // starts), so our page answers first.
  this->base_->add_handler_without_auth(this);
  this->last_activity_ms_ = millis();
}

void JigglerPortal::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Jiggler portal:\n"
                "  Saved networks: %u\n"
                "  Identity: %s\n"
                "  Weak-signal recovery: %s",
                (unsigned) this->nets_.count, this->mouse_->get_device_name().c_str(),
                YESNO(this->weak_recovery_));
}

void JigglerPortal::load_() {
  this->cfg_pref_ = global_preferences->make_preference<Config>(CFG_KEY);
  this->nets_pref_ = global_preferences->make_preference<NetStore>(NETS_KEY);

  Config c{};
  if (this->cfg_pref_.load(&c) && c.magic == CFG_MAGIC) {
    this->cfg_ = c;
  } else {
    this->cfg_ = Config{};
    this->cfg_.magic = CFG_MAGIC;
    this->cfg_.profile = this->def_profile_ < N_PROFILES ? this->def_profile_ : 0;
    this->cfg_.amplitude = this->def_amp_;
    this->cfg_.min_s = this->def_min_;
    this->cfg_.max_s = this->def_max_;
    this->cfg_.dim_s = 60;
    this->cfg_.restart_days = 0;
  }
  // Defend against a corrupt blob: force terminators and sane ranges.
  this->cfg_.name[sizeof(this->cfg_.name) - 1] = 0;
  this->cfg_.mfr[sizeof(this->cfg_.mfr) - 1] = 0;
  this->cfg_.model[sizeof(this->cfg_.model) - 1] = 0;
  if (this->cfg_.profile != CUSTOM_PROFILE && this->cfg_.profile >= N_PROFILES) this->cfg_.profile = 0;
  this->cfg_.amplitude = std::max<uint8_t>(1, std::min<uint8_t>(5, this->cfg_.amplitude));
  this->cfg_.min_s = std::max(10.0f, std::min(3600.0f, this->cfg_.min_s));
  this->cfg_.max_s = std::max(this->cfg_.min_s, std::min(3600.0f, this->cfg_.max_s));

  NetStore n{};
  if (this->nets_pref_.load(&n) && n.magic == NETS_MAGIC && n.count <= MAX_NETS) {
    this->nets_ = n;
    for (auto &r : this->nets_.nets) {
      r.ssid[sizeof(r.ssid) - 1] = 0;
      r.psk[sizeof(r.psk) - 1] = 0;
    }
  } else {
    this->nets_ = NetStore{};
    this->nets_.magic = NETS_MAGIC;
  }
}

void JigglerPortal::save_cfg_() {
  this->cfg_pref_.save(&this->cfg_);
  global_preferences->sync();
}

void JigglerPortal::save_nets_() {
  this->nets_pref_.save(&this->nets_);
  global_preferences->sync();
}

void JigglerPortal::apply_identity_() {
  const Profile *p = this->cfg_.profile < N_PROFILES ? &PROFILES[this->cfg_.profile] : nullptr;
  if (p != nullptr) {
    this->mouse_->set_device_name(p->name);
    this->mouse_->set_manufacturer(p->mfr);
    this->mouse_->set_model(p->model);
    this->mouse_->set_pnp_id(p->src, p->vid, p->pid, p->ver);
  } else {
    this->mouse_->set_device_name(this->cfg_.name[0] ? this->cfg_.name : "Bluetooth Mouse");
    this->mouse_->set_manufacturer(this->cfg_.mfr[0] ? this->cfg_.mfr : "Generic");
    this->mouse_->set_model(this->cfg_.model[0] ? this->cfg_.model : "BT Mouse");
    this->mouse_->set_pnp_id(2, 0x1209, 0x0001, 0x0001);
  }
}

void JigglerPortal::apply_jiggler_() {
  this->mouse_->set_min_interval_s(this->cfg_.min_s);
  this->mouse_->set_max_interval_s(this->cfg_.max_s);
  this->mouse_->set_amplitude(this->cfg_.amplitude);
}

void JigglerPortal::apply_networks_() {
  if (this->nets_.count == 0) return;
  auto *w = wifi::global_wifi_component;
  w->clear_sta();
  w->init_sta(this->nets_.count);
  for (uint8_t i = 0; i < this->nets_.count; i++) {
    wifi::WiFiAP ap;
    ap.set_ssid(this->nets_.nets[i].ssid);
    ap.set_password(this->nets_.nets[i].psk);
    ap.set_hidden(this->nets_.nets[i].hidden != 0);
    w->add_sta(ap);
  }
}

// ---------------------------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------------------------
bool JigglerPortal::wifi_connected() const { return wifi::global_wifi_component->is_connected(); }

bool JigglerPortal::setup_ap_active() const {
  return !wifi::global_wifi_component->is_connected() && wifi::global_wifi_component->is_ap_active();
}

uint8_t JigglerPortal::wifi_bars() const {
  if (!this->wifi_connected()) return 0;
  return bars_from_rssi(wifi::global_wifi_component->wifi_rssi());
}

bool JigglerPortal::dim_wanted() const {
  if (this->cfg_.dim_s == 0) return false;
  return (millis() - this->last_activity_ms_) > (uint32_t) this->cfg_.dim_s * 1000UL;
}

void JigglerPortal::loop() {
  const uint32_t now = millis();
  auto *w = wifi::global_wifi_component;

  if (!this->first_loop_done_) {
    this->first_loop_done_ = true;
    this->mouse_->set_enabled((this->cfg_.flags & 1) != 0);
  }

  // Serve the page on the home network, and on the setup network when it is up.
  if (!this->server_started_ && (w->is_connected() || w->is_ap_active())) {
    this->base_->init();
    this->server_started_ = true;
  }

  // A jiggle is a handful of tiny reports; do not let a roaming scan take the radio off-channel
  // in the middle of one.
  const bool jiggling = this->mouse_->jiggling();
  if (jiggling && !this->jiggle_suppress_) {
    w->request_roaming_suppression();
    this->jiggle_suppress_ = true;
  } else if (!jiggling && this->jiggle_suppress_) {
    w->release_roaming_suppression();
    this->jiggle_suppress_ = false;
  }

  if (this->pending_restart_) return;

  // Wi-Fi up/down transitions.
  const bool connected = w->is_connected();
  if (connected != this->was_connected_) {
    this->was_connected_ = connected;
    this->touch();
    if (connected) {
      this->info_until_ms_ = now + INFO_AFTER_CONNECT_MS;
      this->trying_since_ms_ = 0;
      if (this->reload_after_connect_) {
        // The setup page joined a new network live; restart once so every saved network is loaded.
        this->reload_after_connect_ = false;
        this->set_timeout("reload_nets", 20000, [this]() { this->schedule_restart_(500); });
      }
    }
  }
  if (!connected && this->trying_since_ms_ == 0 && this->nets_.count > 0) this->trying_since_ms_ = now;

  if (now - this->last_snapshot_ms_ >= 1000 || this->last_snapshot_ms_ == 0) {
    this->last_snapshot_ms_ = now;
    this->update_ip_text_();
    this->refresh_snapshot_();
    this->weak_signal_watchdog_(now);

    // Any change in what the OLED shows wakes it from dimming.
    static uint32_t last_sig = 0;
    const uint32_t sig = (this->mouse_->is_ready() ? 1 : 0) | (this->mouse_->has_bond() ? 2 : 0) |
                         (this->mouse_->pairing_active() ? 4 : 0) | (this->mouse_->is_enabled() ? 8 : 0) |
                         (connected ? 16 : 0) | (w->is_ap_active() ? 32 : 0) | (this->ota_active_ ? 64 : 0);
    if (sig != last_sig) {
      last_sig = sig;
      this->touch();
    }

    // Optional housekeeping restart, by uptime rather than clock time (there is no clock).
    if (this->cfg_.restart_days > 0 && !jiggling && !this->ota_active_) {
      const uint64_t up_s = millis_64() / 1000ULL;
      if (up_s > (uint64_t) this->cfg_.restart_days * 86400ULL) {
        ESP_LOGI(TAG, "Scheduled restart after %u day(s) of uptime", (unsigned) this->cfg_.restart_days);
        this->schedule_restart_(200);
      }
    }
  }
}

void JigglerPortal::update_ip_text_() {
  auto *w = wifi::global_wifi_component;
  char buf[network::IP_ADDRESS_BUFFER_SIZE];
  buf[0] = 0;
  if (w->is_connected()) {
    for (auto &ip : network::get_ip_addresses()) {
      if (ip.is_ip4()) {
        ip.str_to(buf);
        break;
      }
    }
  } else if (w->is_ap_active()) {
    w->wifi_soft_ap_ip().str_to(buf);
  }
  strncpy(this->ip_text_, buf, sizeof(this->ip_text_) - 1);
  this->ip_text_[sizeof(this->ip_text_) - 1] = 0;
}

void JigglerPortal::weak_signal_watchdog_(uint32_t now) {
  auto *w = wifi::global_wifi_component;
  if (!this->weak_recovery_ || !w->is_connected()) {
    this->weak_since_ms_ = 0;
    return;
  }
  const int rssi = w->wifi_rssi();
  if (rssi >= WEAK_RSSI_DBM) {
    this->weak_since_ms_ = 0;
    return;
  }
  if (this->weak_since_ms_ == 0) {
    this->weak_since_ms_ = now;
    return;
  }
  if (now - this->weak_since_ms_ < WEAK_FOR_MS) return;
  if (this->last_recovery_ms_ != 0 && now - this->last_recovery_ms_ < RECOVERY_COOLDOWN_MS) return;
  if (this->mouse_->jiggling() || w->is_roaming() || this->ota_active_) return;
  // ESPHome's own roaming gives up after a few tries. Dropping the link makes it go through the
  // normal "connection lost" path, which scans again and skips an access point that just served us
  // badly. Bluetooth is a separate controller and keeps running.
  ESP_LOGW(TAG, "Wi-Fi weak (%d dBm) for %u min; reconnecting to look for a better access point", rssi,
           (unsigned) (WEAK_FOR_MS / 60000UL));
  this->last_recovery_ms_ = now;
  this->weak_since_ms_ = 0;
  w->restart_adapter();
}

// ---------------------------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------------------------
void JigglerPortal::on_button_short() {
  this->touch();
  this->info_until_ms_ = millis() + INFO_AFTER_TAP_MS;
}

void JigglerPortal::on_button_pair() {
  this->touch();
  this->mouse_->start_pairing();
}

void JigglerPortal::on_button_forget() {
  this->touch();
  this->mouse_->forget_host();
}

void JigglerPortal::on_button_wifi_reset() {
  ESP_LOGW(TAG, "Wi-Fi networks cleared from the button; restarting into setup mode");
  this->nets_ = NetStore{};
  this->nets_.magic = NETS_MAGIC;
  this->save_nets_();
  this->schedule_restart_(300);
}

void JigglerPortal::schedule_restart_(uint32_t ms) {
  if (this->pending_restart_) return;
  this->pending_restart_ = true;
  this->set_timeout("restart", ms, []() { App.safe_reboot(); });
}

// ---------------------------------------------------------------------------------------------
// Networks
// ---------------------------------------------------------------------------------------------
bool JigglerPortal::add_network_(const std::string &ssid, const std::string &psk, bool hidden, std::string &err) {
  if (ssid.empty() || ssid.size() > 32) {
    err = "Network names are 1 to 32 characters.";
    return false;
  }
  if (!psk.empty() && (psk.size() < 8 || psk.size() > 63)) {
    err = "Wi-Fi passwords are 8 to 63 characters.";
    return false;
  }
  int at = -1;
  for (uint8_t i = 0; i < this->nets_.count; i++)
    if (ssid == this->nets_.nets[i].ssid) at = i;
  if (at < 0) {
    if (this->nets_.count >= MAX_NETS) {
      err = "The jiggler remembers up to 5 networks. Remove one first.";
      return false;
    }
    at = this->nets_.count++;
  }
  copy_str(this->nets_.nets[at].ssid, sizeof(NetRec::ssid), ssid);
  copy_str(this->nets_.nets[at].psk, sizeof(NetRec::psk), psk);
  this->nets_.nets[at].hidden = hidden ? 1 : 0;
  this->save_nets_();
  ESP_LOGI(TAG, "Saved network '%s' (%u total)", ssid.c_str(), (unsigned) this->nets_.count);

  if (!this->wifi_connected()) {
    // Same thing ESPHome's own portal does: try the new network right now, keep the access point
    // up while it tries. Once it connects we restart once to load every saved network.
    wifi::WiFiAP ap;
    ap.set_ssid(this->nets_.nets[at].ssid);
    ap.set_password(this->nets_.nets[at].psk);
    ap.set_hidden(hidden);
    wifi::global_wifi_component->set_sta(ap);
    this->reload_after_connect_ = this->nets_.count > 1;
    this->trying_since_ms_ = millis();
  } else {
    this->schedule_restart_();
  }
  return true;
}

bool JigglerPortal::remove_network_(const std::string &ssid) {
  int at = -1;
  for (uint8_t i = 0; i < this->nets_.count; i++)
    if (ssid == this->nets_.nets[i].ssid) at = i;
  if (at < 0) return false;
  for (uint8_t i = at; i + 1 < this->nets_.count; i++) this->nets_.nets[i] = this->nets_.nets[i + 1];
  this->nets_.count--;
  memset(&this->nets_.nets[this->nets_.count], 0, sizeof(NetRec));
  this->save_nets_();
  ESP_LOGI(TAG, "Removed a saved network (%u left)", (unsigned) this->nets_.count);
  this->schedule_restart_();
  return true;
}

// ---------------------------------------------------------------------------------------------
// State snapshot (built on the main loop, read by the HTTP task)
// ---------------------------------------------------------------------------------------------
void JigglerPortal::refresh_snapshot_() {
  auto *w = wifi::global_wifi_component;
  std::string o;
  o.reserve(900);
  o += "{\"fw\":";
  jstr(o, this->fw_version_.c_str());
  o += ",\"esphome\":";
  jstr(o, ESPHOME_VERSION);
  o += ",\"name\":";
  jstr(o, App.get_name().c_str());
  o += ",\"tag\":";
  jstr(o, this->tag_);
  o += ",\"up\":";
  jnum(o, (long) (millis_64() / 1000ULL));
  o += ",\"heap\":";
  jnum(o, (long) esp_get_free_heap_size());
  o += ",\"on\":";
  o += this->mouse_->is_enabled() ? "true" : "false";

  o += ",\"bt\":{\"link\":";
  o += this->mouse_->is_link_connected() ? "true" : "false";
  o += ",\"ready\":";
  o += this->mouse_->is_ready() ? "true" : "false";
  o += ",\"bond\":";
  o += this->mouse_->has_bond() ? "true" : "false";
  o += ",\"pair\":";
  o += this->mouse_->pairing_active() ? "true" : "false";
  o += ",\"left\":";
  jnum(o, (long) this->mouse_->pairing_remaining_s());
  o += ",\"idle\":";
  jnum(o, (long) this->mouse_->seconds_since_disconnect());
  o += "}";

  const uint32_t ago = this->mouse_->ms_since_jiggle();
  o += ",\"j\":{\"next\":";
  jnum(o, (long) this->mouse_->next_jiggle_in_s());
  o += ",\"prog\":";
  jfloat(o, this->mouse_->interval_progress(), 3);
  o += ",\"count\":";
  jnum(o, (long) this->mouse_->jiggle_count());
  o += ",\"ago\":";
  jnum(o, ago == UINT32_MAX ? -1L : (long) (ago / 1000));
  o += ",\"min\":";
  jnum(o, (long) this->cfg_.min_s);
  o += ",\"max\":";
  jnum(o, (long) this->cfg_.max_s);
  o += ",\"amp\":";
  jnum(o, (long) this->cfg_.amplitude);
  o += "}";

  const bool connected = w->is_connected();
  o += ",\"w\":{\"up\":";
  o += connected ? "true" : "false";
  o += ",\"ap\":";
  o += w->is_ap_active() ? "true" : "false";
  o += ",\"saved\":";
  jnum(o, (long) this->nets_.count);
  o += ",\"ip\":";
  jstr(o, this->ip_text_);
  o += ",\"host\":";
  {
    std::string h = App.get_name().c_str();
    h += ".local";
    jstr(o, h.c_str());
  }
  if (connected) {
    char ssid[wifi::SSID_BUFFER_SIZE];
    w->wifi_ssid_to(ssid);
    o += ",\"ssid\":";
    jstr(o, ssid);
    const int rssi = w->wifi_rssi();
    o += ",\"rssi\":";
    jnum(o, rssi);
    o += ",\"bars\":";
    jnum(o, bars_from_rssi(rssi));
    o += ",\"ch\":";
    jnum(o, (long) w->get_wifi_channel());
    o += ",\"roam\":";
    o += w->is_roaming() ? "true" : "false";
  } else if (this->trying_since_ms_ != 0) {
    o += ",\"trying\":";
    jnum(o, (long) ((millis() - this->trying_since_ms_) / 1000));
  }
  o += "}";

  o += ",\"nets\":[";
  for (uint8_t i = 0; i < this->nets_.count; i++) {
    if (i) o += ',';
    o += "{\"ssid\":";
    jstr(o, this->nets_.nets[i].ssid);
    o += ",\"hidden\":";
    o += this->nets_.nets[i].hidden ? "true" : "false";
    o += "}";
  }
  o += "]";

  o += ",\"id\":{\"profile\":";
  jnum(o, this->cfg_.profile == CUSTOM_PROFILE ? -1L : (long) this->cfg_.profile);
  o += ",\"name\":";
  jstr(o, this->mouse_->get_device_name().c_str());
  o += ",\"cname\":";
  jstr(o, this->cfg_.name);
  o += ",\"cmfr\":";
  jstr(o, this->cfg_.mfr);
  o += ",\"cmodel\":";
  jstr(o, this->cfg_.model);
  o += "}";

  o += ",\"cfg\":{\"dim\":";
  jnum(o, (long) this->cfg_.dim_s);
  o += ",\"days\":";
  jnum(o, (long) this->cfg_.restart_days);
  o += ",\"display\":";
  o += (this->cfg_.flags & 2) ? "false" : "true";
  o += "}}";

  LockGuard guard{this->lock_};
  this->snapshot_ = std::move(o);
}

// ---------------------------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------------------------
bool JigglerPortal::canHandle(AsyncWebServerRequest *request) const {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const StringRef url = request->url_to(buf);
  const std::string_view u(url.c_str(), url.size());
  if (u.rfind("/api/", 0) == 0) return true;
  if (request->method() != HTTP_GET) return false;
  if (u == "/" || u == "/index.html") return true;
  // While the setup network is up, answer every other page with ours: that is how phones decide
  // to open the "sign in to Wi-Fi" sheet. Leave the firmware upload page alone.
  if (wifi::global_wifi_component->is_ap_active()) return u.rfind("/update", 0) != 0;
  return false;
}

void JigglerPortal::send_json_(AsyncWebServerRequest *request, const std::string &body, int code) {
  auto *r = request->beginResponse(code, "application/json", body);
  r->addHeader("Cache-Control", "no-store");
  request->send(r);
}

void JigglerPortal::send_ok_(AsyncWebServerRequest *request) { this->send_json_(request, "{\"ok\":true}"); }

void JigglerPortal::send_err_(AsyncWebServerRequest *request, const char *msg, int code) {
  std::string o = "{\"ok\":false,\"error\":";
  jstr(o, msg);
  o += "}";
  this->send_json_(request, o, code);
}

void JigglerPortal::api_scan_(AsyncWebServerRequest *request) {
  struct Row {
    std::string ssid;
    int rssi;
    bool lock;
    int ch;
  };
  std::vector<Row> rows;
  {
    wifi::ScanResultsLock lock(wifi::global_wifi_component);
    for (const auto &r : wifi::global_wifi_component->get_scan_result()) {
      if (r.get_is_hidden() || r.get_ssid().empty()) continue;
      const std::string ssid(r.get_ssid().c_str(), r.get_ssid().size());
      bool merged = false;
      for (auto &x : rows) {
        if (x.ssid == ssid) {
          if (r.get_rssi() > x.rssi) {
            x.rssi = r.get_rssi();
            x.ch = r.get_channel();
          }
          merged = true;
          break;
        }
      }
      if (!merged) rows.push_back({ssid, r.get_rssi(), r.get_with_auth(), r.get_channel()});
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.rssi > b.rssi; });
  if (rows.size() > 24) rows.resize(24);
  std::string o = "{\"aps\":[";
  bool first = true;
  for (const auto &x : rows) {
    if (!first) o += ',';
    first = false;
    bool saved = false;
    for (uint8_t i = 0; i < this->nets_.count; i++)
      if (x.ssid == this->nets_.nets[i].ssid) saved = true;
    o += "{\"ssid\":";
    jstr(o, x.ssid.c_str());
    o += ",\"rssi\":";
    jnum(o, x.rssi);
    o += ",\"bars\":";
    jnum(o, bars_from_rssi(x.rssi));
    o += ",\"lock\":";
    o += x.lock ? "true" : "false";
    o += ",\"ch\":";
    jnum(o, x.ch);
    o += ",\"saved\":";
    o += saved ? "true" : "false";
    o += "}";
  }
  o += "]}";
  this->send_json_(request, o);
}

void JigglerPortal::api_profiles_(AsyncWebServerRequest *request) {
  std::string o = "[";
  for (size_t i = 0; i < N_PROFILES; i++) {
    if (i) o += ',';
    char ids[24];
    snprintf(ids, sizeof(ids), "%04X:%04X", PROFILES[i].vid, PROFILES[i].pid);
    o += "{\"i\":";
    jnum(o, (long) i);
    o += ",\"label\":";
    jstr(o, PROFILES[i].label);
    o += ",\"name\":";
    jstr(o, PROFILES[i].name);
    o += ",\"mfr\":";
    jstr(o, PROFILES[i].mfr);
    o += ",\"ids\":";
    jstr(o, ids);
    o += "}";
  }
  o += "]";
  this->send_json_(request, o);
}

void JigglerPortal::handleRequest(AsyncWebServerRequest *request) {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const StringRef url = request->url_to(buf);
  const std::string_view u(url.c_str(), url.size());
  if (u.rfind("/api/", 0) == 0) {
    this->handle_api_(request, url.c_str() + 5);
    return;
  }
  if (u == "/favicon.ico") {
    request->send(204);
    return;
  }
  auto *r = request->beginResponse(200, "text/html", this->page_, this->page_len_);
  r->addHeader("Content-Encoding", "gzip");
  r->addHeader("Cache-Control", "no-store");
  request->send(r);
}

void JigglerPortal::handle_api_(AsyncWebServerRequest *request, const char *path) {
  const std::string_view p(path);
  const bool post = request->method() == HTTP_POST;

  if (!post) {
    if (p == "state") {
      std::string copy;
      {
        LockGuard guard{this->lock_};
        copy = this->snapshot_;
      }
      if (copy.empty()) copy = "{}";
      this->send_json_(request, copy);
    } else if (p == "scan") {
      this->api_scan_(request);
    } else if (p == "profiles") {
      this->api_profiles_(request);
    } else {
      this->send_err_(request, "Unknown request.", 404);
    }
    return;
  }

  // ---- POST actions. Validate here, change state on the main loop. ----
  if (p == "enable") {
    const bool on = request->arg("on") == "1";
    this->defer([this, on]() {
      if (on)
        this->cfg_.flags |= 1;
      else
        this->cfg_.flags &= ~1;
      this->save_cfg_();
      this->mouse_->set_enabled(on);
      this->touch();
    });
    this->send_ok_(request);
  } else if (p == "timing") {
    const float mn = atof(request->arg("min").c_str());
    const float mx = atof(request->arg("max").c_str());
    const int amp = atoi(request->arg("amp").c_str());
    if (mn < 10 || mn > 3600 || mx < 10 || mx > 3600 || amp < 1 || amp > 5) {
      this->send_err_(request, "Use 10 to 3600 seconds and a size of 1 to 5 pixels.");
      return;
    }
    const float hi = std::max(mn, mx);
    this->defer([this, mn, hi, amp]() {
      this->cfg_.min_s = mn;
      this->cfg_.max_s = hi;
      this->cfg_.amplitude = (uint8_t) amp;
      this->save_cfg_();
      this->apply_jiggler_();
    });
    this->send_ok_(request);
  } else if (p == "jiggle") {
    this->defer([this]() {
      this->mouse_->jiggle_now();
      this->touch();
    });
    this->send_ok_(request);
  } else if (p == "pair") {
    this->defer([this]() { this->on_button_pair(); });
    this->send_ok_(request);
  } else if (p == "pair_stop") {
    this->defer([this]() { this->mouse_->stop_pairing(); });
    this->send_ok_(request);
  } else if (p == "forget") {
    this->defer([this]() { this->on_button_forget(); });
    this->send_ok_(request);
  } else if (p == "profile") {
    const std::string id = request->arg("id");
    const bool custom = id == "custom";
    const int idx = custom ? -1 : atoi(id.c_str());
    std::string name = trimmed(request->arg("name")), mfr = trimmed(request->arg("mfr")),
                model = trimmed(request->arg("model"));
    if (!custom && (idx < 0 || idx >= (int) N_PROFILES || id.empty())) {
      this->send_err_(request, "Pick one of the listed mice.");
      return;
    }
    if (custom) {
      if (name.empty() || name.size() > 24 || mfr.size() > 30 || model.size() > 30 || !printable(name) ||
          !printable(mfr) || !printable(model)) {
        this->send_err_(request, "Give the mouse a name of up to 24 characters.");
        return;
      }
    }
    this->defer([this, custom, idx, name, mfr, model]() {
      this->cfg_.profile = custom ? CUSTOM_PROFILE : (uint8_t) idx;
      if (custom) {
        copy_str(this->cfg_.name, sizeof(this->cfg_.name), name);
        copy_str(this->cfg_.mfr, sizeof(this->cfg_.mfr), mfr);
        copy_str(this->cfg_.model, sizeof(this->cfg_.model), model);
      }
      this->save_cfg_();
      // A new identity means the old pairing no longer matches: drop it, restart, pair again.
      this->mouse_->clear_bonds();
      this->schedule_restart_();
    });
    this->send_ok_(request);
  } else if (p == "scan_start") {
    // Only safe to start by hand while no network is saved: otherwise ESPHome's own connection
    // logic is already scanning on a schedule and owns the radio.
    this->defer([this]() {
      auto *w = wifi::global_wifi_component;
      if (!w->is_connected() && w->is_ap_active() && this->nets_.count == 0) w->start_scanning();
    });
    this->send_ok_(request);
  } else if (p == "wifi_add") {
    const std::string ssid = request->arg("ssid");
    const std::string psk = request->arg("psk");
    const bool hidden = request->arg("hidden") == "1";
    std::string err;
    // Validate now so the page can show the reason; the actual change runs on the main loop.
    if (ssid.empty() || ssid.size() > 32) {
      this->send_err_(request, "Network names are 1 to 32 characters.");
      return;
    }
    if (!psk.empty() && (psk.size() < 8 || psk.size() > 63)) {
      this->send_err_(request, "Wi-Fi passwords are 8 to 63 characters.");
      return;
    }
    bool known = false;
    for (uint8_t i = 0; i < this->nets_.count; i++)
      if (ssid == this->nets_.nets[i].ssid) known = true;
    if (!known && this->nets_.count >= MAX_NETS) {
      this->send_err_(request, "The jiggler remembers up to 5 networks. Remove one first.");
      return;
    }
    this->defer([this, ssid, psk, hidden]() {
      std::string e;
      this->add_network_(ssid, psk, hidden, e);
    });
    this->send_ok_(request);
  } else if (p == "wifi_remove") {
    const std::string ssid = request->arg("ssid");
    this->defer([this, ssid]() { this->remove_network_(ssid); });
    this->send_ok_(request);
  } else if (p == "wifi_reset") {
    this->defer([this]() { this->on_button_wifi_reset(); });
    this->send_ok_(request);
  } else if (p == "device") {
    const int dim = atoi(request->arg("dim").c_str());
    const int days = atoi(request->arg("days").c_str());
    const bool display = request->arg("display") != "0";
    if (dim < 0 || dim > 3600 || days < 0 || days > 365) {
      this->send_err_(request, "Dim time is 0 to 3600 seconds and restart is 0 to 365 days.");
      return;
    }
    this->defer([this, dim, days, display]() {
      this->cfg_.dim_s = (uint16_t) dim;
      this->cfg_.restart_days = (uint16_t) days;
      if (display)
        this->cfg_.flags &= ~2;
      else
        this->cfg_.flags |= 2;
      this->save_cfg_();
      this->touch();
    });
    this->send_ok_(request);
  } else if (p == "restart") {
    this->defer([this]() { this->schedule_restart_(); });
    this->send_ok_(request);
  } else if (p == "factory") {
    this->defer([this]() {
      this->cfg_ = Config{};
      this->cfg_.magic = 0;  // invalid on purpose: defaults are rebuilt on the next boot
      this->save_cfg_();
      this->nets_ = NetStore{};
      this->nets_.magic = NETS_MAGIC;
      this->save_nets_();
      this->mouse_->clear_bonds();
      this->schedule_restart_();
    });
    this->send_ok_(request);
  } else {
    this->send_err_(request, "Unknown request.", 404);
  }
}

}  // namespace jiggler_portal
}  // namespace esphome

#endif  // USE_JIGGLER_PORTAL
