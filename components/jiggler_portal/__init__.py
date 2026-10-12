"""Phone-friendly setup and control page for the standalone BLE mouse jiggler.

Runs next to ESPHome's own ``captive_portal`` (which keeps doing the heavy lifting: the setup
access point, the DNS redirect, scanning, retries and roaming) and answers first, so the page
you see is ours. It also stores the jiggler settings and a list of Wi-Fi networks in flash,
because there is no Home Assistant to hold them.
"""

import gzip
from pathlib import Path

import esphome.codegen as cg
from esphome.components import wifi
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.components import web_server_base
import esphome.config_validation as cv
from esphome.const import CONF_ID

AUTO_LOAD = ["web_server_base", "captive_portal", "json"]
DEPENDENCIES = ["wifi", "ble_hid_mouse", "network"]
CODEOWNERS = []

CONF_MOUSE_ID = "mouse_id"
CONF_RAW_DATA_ID = "raw_data_id"
CONF_DEFAULT_MIN = "default_min_interval"
CONF_DEFAULT_MAX = "default_max_interval"
CONF_DEFAULT_AMPLITUDE = "default_amplitude"
CONF_DEFAULT_PROFILE = "default_profile"
CONF_WEAK_SIGNAL_RECOVERY = "weak_signal_recovery"
CONF_FIRMWARE_VERSION = "firmware_version"

jiggler_portal_ns = cg.esphome_ns.namespace("jiggler_portal")
JigglerPortal = jiggler_portal_ns.class_("JigglerPortal", cg.Component)

ble_hid_mouse_ns = cg.esphome_ns.namespace("ble_hid_mouse")
BLEHIDMouse = ble_hid_mouse_ns.class_("BLEHIDMouse", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(JigglerPortal),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
        cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
        cv.Required(CONF_MOUSE_ID): cv.use_id(BLEHIDMouse),
        cv.Optional(CONF_DEFAULT_MIN, default=120): cv.int_range(min=10, max=3600),
        cv.Optional(CONF_DEFAULT_MAX, default=180): cv.int_range(min=10, max=3600),
        cv.Optional(CONF_DEFAULT_AMPLITUDE, default=2): cv.int_range(min=1, max=5),
        cv.Optional(CONF_DEFAULT_PROFILE, default=0): cv.int_range(min=0, max=20),
        # Last-resort recovery for a stuck, weak connection: see jiggler_portal.cpp.
        cv.Optional(CONF_WEAK_SIGNAL_RECOVERY, default=True): cv.boolean,
        cv.Optional(CONF_FIRMWARE_VERSION, default="1.0.0"): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    import esphome.final_validate as fv

    full = fv.full_config.get()
    wifi_conf = full.get("wifi", {})
    if "ap" not in wifi_conf:
        raise cv.Invalid(
            "jiggler_portal needs `ap:` under `wifi:` so the device can offer a setup network"
        )
    if wifi_conf.get("networks") or wifi_conf.get("ssid"):
        raise cv.Invalid(
            "jiggler_portal stores Wi-Fi networks itself; remove `ssid:`/`networks:` from `wifi:`"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    paren = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    mouse = await cg.get_variable(config[CONF_MOUSE_ID])
    var = cg.new_Pvariable(config[CONF_ID], paren, mouse)
    await cg.register_component(var, config)

    html = (Path(__file__).parent / "web" / "index.html").read_bytes()
    packed = gzip.compress(html, compresslevel=9, mtime=0)
    cg.add_define("USE_JIGGLER_PORTAL")
    prog = cg.progmem_array(config[CONF_RAW_DATA_ID], list(packed))
    cg.add(var.set_page(prog, len(packed)))

    cg.add(var.set_defaults(
        config[CONF_DEFAULT_MIN],
        config[CONF_DEFAULT_MAX],
        config[CONF_DEFAULT_AMPLITUDE],
        config[CONF_DEFAULT_PROFILE],
    ))
    cg.add(var.set_weak_signal_recovery(config[CONF_WEAK_SIGNAL_RECOVERY]))
    cg.add(var.set_firmware_version(config[CONF_FIRMWARE_VERSION]))

    # Full scan results (not just the configured network) and a lock, because the web server
    # reads them from its own task.
    wifi.request_wifi_scan_results()
    wifi.request_wifi_scan_results_lock()
    # Lets us pause roaming scans for the second a jiggle takes to send.
    wifi.enable_runtime_roaming_suppression()
