"""BLE HID-over-GATT mouse + jiggle scheduler for ESPHome (ESP32, ESP-IDF, Bluedroid).

This component rides on ESPHome's own ``esp32_ble`` component (it does not take over the
Bluetooth controller). It registers its own GATT application through esp32_ble's GATTS/GAP
callback fan-out and drives advertising itself, so it can use encrypted characteristics,
LE Secure Connections "Just Works" bonding, and a pairing-mode / single-host policy that
the stock ``esp32_ble_server`` cannot express.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import binary_sensor, esp32_ble
from esphome.components.esp32 import add_idf_sdkconfig_option, request_bluetooth
from esphome.components.esp32_ble import CONF_BLE_ID
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODEL, CONF_TRIGGER_ID

AUTO_LOAD = ["esp32_ble", "binary_sensor"]
DEPENDENCIES = ["esp32"]
CODEOWNERS = []

CONF_MANUFACTURER = "manufacturer"
CONF_VENDOR_ID_SOURCE = "vendor_id_source"
CONF_VENDOR_ID = "vendor_id"
CONF_PRODUCT_ID = "product_id"
CONF_PRODUCT_VERSION = "product_version"
CONF_PAIRING_TIMEOUT = "pairing_timeout"
CONF_BATTERY_LEVEL = "battery_level"
CONF_CONNECTED = "connected"
CONF_LINK = "link"
CONF_BONDED = "bonded"
CONF_PAIRING = "pairing"
CONF_ON_JIGGLE = "on_jiggle"

ble_hid_mouse_ns = cg.esphome_ns.namespace("ble_hid_mouse")
BLEHIDMouse = ble_hid_mouse_ns.class_("BLEHIDMouse", cg.Component)
JiggleTrigger = ble_hid_mouse_ns.class_("JiggleTrigger", automation.Trigger.template())

VENDOR_ID_SOURCES = {"bluetooth": 1, "usb": 2}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BLEHIDMouse),
        cv.GenerateID(CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
        cv.Optional(CONF_MANUFACTURER, default="Logitech"): cv.All(
            cv.string, cv.Length(min=1, max=30)
        ),
        cv.Optional(CONF_MODEL, default="MX Keys Mini"): cv.All(
            cv.string, cv.Length(min=1, max=30)
        ),
        cv.Optional(CONF_VENDOR_ID_SOURCE, default="usb"): cv.enum(
            VENDOR_ID_SOURCES, lower=True
        ),
        cv.Optional(CONF_VENDOR_ID, default=0x046D): cv.hex_uint16_t,
        cv.Optional(CONF_PRODUCT_ID, default=0xB369): cv.hex_uint16_t,
        cv.Optional(CONF_PRODUCT_VERSION, default=0x0001): cv.hex_uint16_t,
        cv.Optional(CONF_PAIRING_TIMEOUT, default="120s"): cv.All(
            cv.positive_time_period_seconds,
            cv.Range(min=cv.TimePeriod(seconds=10), max=cv.TimePeriod(seconds=900)),
        ),
        cv.Optional(CONF_BATTERY_LEVEL, default=100): cv.int_range(min=0, max=100),
        cv.Optional(CONF_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class="connectivity"
        ),
        cv.Optional(CONF_LINK): binary_sensor.binary_sensor_schema(
            device_class="connectivity"
        ),
        cv.Optional(CONF_BONDED): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_PAIRING): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_ON_JIGGLE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(JiggleTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    # The device name that central hosts see is the esp32_ble `name:` option.
    import esphome.final_validate as fv

    ble = fv.full_config.get().get("esp32_ble", {})
    if ble.get("io_capability", "none") != "none":
        raise cv.Invalid(
            "esp32_ble: io_capability must be 'none' for Just Works pairing "
            "with ble_hid_mouse"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_BLE_ID])
    cg.add(var.set_parent(parent))

    from esphome.core import CORE

    ble_conf = CORE.config.get("esp32_ble", {})
    device_name = ble_conf.get("name") or CORE.name
    cg.add(var.set_device_name(device_name[:20]))
    cg.add(var.set_manufacturer(config[CONF_MANUFACTURER]))
    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(
        var.set_pnp_id(
            config[CONF_VENDOR_ID_SOURCE],
            config[CONF_VENDOR_ID],
            config[CONF_PRODUCT_ID],
            config[CONF_PRODUCT_VERSION],
        )
    )
    cg.add(var.set_pairing_timeout_ms(config[CONF_PAIRING_TIMEOUT].total_milliseconds))
    cg.add(var.set_battery_level(config[CONF_BATTERY_LEVEL]))

    if CONF_CONNECTED in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_CONNECTED])
        cg.add(var.set_connected_sensor(sens))
    if CONF_LINK in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_LINK])
        cg.add(var.set_link_sensor(sens))
    if CONF_BONDED in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_BONDED])
        cg.add(var.set_bonded_sensor(sens))
    if CONF_PAIRING in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_PAIRING])
        cg.add(var.set_pairing_sensor(sens))

    for conf in config.get(CONF_ON_JIGGLE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    # esp32_ble only forwards GATTS events (and enables the GATT server in the
    # Bluedroid sdkconfig) when it believes a GATT server exists. We are our own
    # GATTS application, so flip those switches ourselves.
    cg.add_define("USE_ESP32_BLE_SERVER")
    add_idf_sdkconfig_option("CONFIG_BT_GATTS_ENABLE", True)
    # Bluedroid is the stack ESPHome uses; make sure SMP/bonding is compiled in and
    # persisted to NVS.
    add_idf_sdkconfig_option("CONFIG_BT_BLE_SMP_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_BT_SMP_ENABLE", True)
    request_bluetooth()

    # Hook into esp32_ble's callback fan-out (also sizes the handler slot arrays).
    esp32_ble.register_gap_event_handler(parent, var)
    esp32_ble.register_gatts_event_handler(parent, var)
