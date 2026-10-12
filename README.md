# BLE Mouse Jiggler (ESPHome, ESP32-C3 + 0.42" OLED)

> **No Home Assistant?** See [README-standalone.md](README-standalone.md): phone setup page, Wi-Fi search and join, selectable mouse identity.

An ESP32-C3 board pairs with your Mac over Bluetooth LE as a standard HID mouse. When enabled it nudges the cursor 1-2 px and straight back at a random interval (default 2-3 minutes), reports its Bluetooth state to Home Assistant (HA), shows status as icons on the OLED, and keeps working with no Wi-Fi or HA.

It is **mouse only**: reports never press a button, scroll, or send a key.

## Design notes (Phase 0 decision)

Target stack: ESPHome **2026.9.1** (latest stable on PyPI), ESP-IDF framework, Bluedroid (the only BLE stack ESPHome's `esp32_ble` supports).

- **(a) Community component:** the closest hit is a BLE *keyboard* external component (HomeOps/esphome-blekeyboard). Nothing found supports a HID mouse with bonding on ESPHome 2026.x, and others are standalone ESP-IDF/Arduino projects, not ESPHome components. Not usable.
- **(b) Custom HOGP component on ESPHome's stack: chosen.** ESPHome's stock `esp32_ble_server` can't mark characteristics as encrypted-only, can't set a report map with the right permissions, and owns advertising with no pairing-mode concept. So `components/ble_hid_mouse/` registers its **own GATT application** on the same Bluedroid stack, through `esp32_ble`'s GAP/GATTS callback fan-out. It builds the HID/Battery/Device Information services itself and drives its own advertising. It does not touch the controller, so Wi-Fi, API and OTA run unmodified next to it.
- **(c) Taking over the BT stack:** not needed.

Result: compiles with Wi-Fi + API + OTA on the C3 (numbers below). Whether it behaves well with macOS is **untested until you try it on the device**.

Other choices:
- "Bluetooth connected" means *link up, encrypted, and the Mac subscribed to input reports*, i.e. the state in which a jiggle can actually be delivered. A diagnostic **Bluetooth link** sensor shows the raw link state; if "Bluetooth link" is on but "Bluetooth connected" stays off, the Mac connected but is not listening (re-pair).
- Single-host policy: a new pairing is only accepted in pairing mode, and accepting one first erases any existing bond.
- Advertising when bonded but disconnected: fast (30-60 ms) for 30 s after a disconnect, then slow (500-750 ms) indefinitely. When there is no bond and pairing mode is off, the device does not advertise at all.

## What was built and what was verified

| Item | Status |
|---|---|
| `esphome config jiggler.yaml` | **Passes**, no warnings |
| `esphome compile jiggler.yaml` | **Succeeds**, no compiler warnings |
| `tools/simulate_jitter.py` | **Passes** (see below) |
| Pairing / bonding with a real Mac | **Not verified** (no hardware) |
| Reconnect after Mac sleep, ESP reboot, OTA | **Not verified** |
| HID reports moving a real cursor | **Not verified** |
| OLED pins, controller, offsets, polarity | **Not verified** (pins are common defaults, not confirmed for your board) |
| OLED icon appearance | **Not verified**; drawn with primitives, never rendered |
| Wi-Fi + BLE coexistence stability over time | **Not verified** |
| Home Assistant entities, example automations | Not run against a live HA (YAML parses; entity IDs assume the device name "BLE Jiggler") |

**Build footprint** (ESPHome 2026.9.1, ESP32-C3, 4 MB flash):
- RAM: 138,332 of 321,296 bytes (43.1%).
- Flash: app 1,230,482 of 1,835,008 bytes per OTA slot (67.1%, about 600 KB free). OTA uses two slots (`0x1C0000` each).
- NVS (settings + Bluetooth bonds): 448 KB.

**Simulation** (`python3 tools/simulate_jitter.py`) models the scheduler logic from the C++ code and asserts: every interval is within [min, max]; every jiggle nets exactly (0, 0); amplitude stays within 1..configured (hard range 1-5); no reports while disconnected, and skipped jiggles are not queued or replayed; a jiggle cut short by a disconnect is undone on reconnect; min > max clamps to max = min; button and wheel bytes are always 0. It tests scheduling only, not Bluetooth.

## First-time install

**Prerequisites:** either the Home Assistant **ESPHome Device Builder** add-on, or `pip install esphome` (2026.9 or newer; the component uses APIs of that release).

**Option A: UI only, no file copying (recommended).** The component is published at <https://github.com/witoldwitkowski/esphome-ble-jiggler>, and `jiggler.yaml` already fetches it from there. In HA open **ESPHome Device Builder → + New Device → Skip** (or any name), then **Edit**, paste the contents of `jiggler.yaml`, and add the four keys from `secrets.example.yaml` via the **Secrets** button (top right). Nothing else to copy; ESPHome downloads the component itself.

**Option B: local files.** In `jiggler.yaml` switch `external_components` to the commented-out local block, then copy `jiggler.yaml` and `components/` into the ESPHome config folder (`/config/esphome/`; Samba, SSH or File editor add-ons work), and create `secrets.yaml` from `secrets.example.yaml`.

Generate the API key with `openssl rand -base64 32` (or any 32-byte base64 value).

3. **Bring-up, step 1: find the OLED.** Flash and open the logs. With `i2c: scan: true` you will see `Found i2c device at address 0x3C`. If nothing is found, the pins in `substitutions:` are wrong for your board.
4. **First flash over USB-C.**
   - Put the board in download mode: hold **BOOT**, tap **RST**, release BOOT.
   - Device Builder: **Install → Plug into this computer** (needs Chrome or Edge, WebSerial). Or: `esphome run jiggler.yaml`.
   - The C3's USB Serial/JTAG port re-enumerates when it resets. If the first attempt fails mid-way, run it again.
5. **Subsequent updates** go over Wi-Fi (OTA). Bluetooth bonds live in NVS and survive OTA.

### Display bring-up test
1. In `substitutions:` set `oled_test_pattern: "true"` and flash. The display shows a border, a diagonal cross, and a solid 4x4 block in each of the four corners of the 72x40 window.
2. All four corner blocks must be visible with no clipping. If the image is shifted or cut off, adjust `oled_offset_x` / `oled_offset_y` (try 0, then 28/24 style offsets), or `oled_model` (`SSD1306 72x40` is a native ESPHome model; `SH1106 128x64` is the next thing to try).
3. Upside down or mirrored: toggle `oled_flip_x` / `oled_flip_y`.
4. Set `oled_test_pattern` back to `"false"`.

## Restarts and memory

- **Restart** button (HA: `button.press` on `button.ble_jiggler_restart`): reboot now. Schedule it with `ha/automation_scheduled_restart.yaml`.
- **Nightly reboot** switch (off by default): reboots at `nightly_reboot_hour`:`nightly_reboot_minute` (default 04:00, HA time). It needs HA time sync and skips if a jiggle or OTA is in progress. Bonds and settings survive; the Mac reconnects by itself.
- **Free heap** and **Largest free heap block** diagnostics (every 60 s) let you see whether memory actually drifts downward over days before deciding you need a reboot. No leak is known; this is a safety net.
- **Pairing mode** is the **Start pairing mode** button (`button.ble_jiggler_start_pairing_mode`). Call it with `button.press`, e.g. from an automation.

## Adding it to Home Assistant

- Normally HA shows a **Discovered** notification for the new ESPHome device. Click **Configure** and enter the API encryption key from `secrets.yaml`.
- Otherwise: Settings → Devices & Services → Add Integration → **ESPHome**, enter the hostname (`ble-jiggler.local`) or IP, then the key.
- Entities appear on the **BLE Jiggler** device: controls (Jiggler enabled, intervals, amplitude, buttons), status, and diagnostics.
- Put `ha/dashboard_card.yaml` into a dashboard (Add card → Manual).
- Put the two automations (`ha/automation_disconnect_alert.yaml`, `ha/automation_schedule_enable.yaml`) in Settings → Automations → ⋮ → Edit in YAML. The schedule one needs a **Schedule helper** named "Jiggler hours". Entity IDs assume the default naming; if HA names them differently, adjust.

## Pairing with the M1 Mac

1. Put the jiggler in **pairing mode**: HA button **Start pairing mode**, or hold **BOOT for 3 s** and release. It advertises for 120 s (OLED shows the Bluetooth rune with animated waves).
2. Mac: **System Settings → Bluetooth**. Find **MX Anywhere 3** (or your `ble_name`) under *Nearby Devices* and click **Connect**.
3. Check that HA's **Bluetooth connected** turns on and the OLED shows the solid Bluetooth icon.
4. If the Mac had cached an older pairing (common): in macOS Bluetooth settings click the **ⓘ** next to the device → **Forget This Device**; in HA press **Forget paired host** (or hold BOOT 10 s); then pair again from step 1.
5. macOS may list the device as "Mouse" in Mouse settings. A mouse should not trigger the Keyboard Setup Assistant; if it appears, it can be dismissed.
6. When the Mac sleeps the BLE link drops and the HA sensor flips to off; the jiggler keeps advertising and the Mac reconnects on wake. No jiggles are queued while it is away.

## Power note

If the board is powered from the Mac's USB port, power may be cut or reduced when the Mac sleeps. A wall USB adapter is more reliable for HA control.

## Troubleshooting

- **Blank or shifted OLED:** run the bring-up test above; check the I2C scan log for `0x3C`.
- **No BLE advertising:** it advertises only in pairing mode, or when a bond exists and the host is disconnected. Press **Start pairing mode**. Check the log for `GATT services started` (the GATT table built) and `Advertising (...)`.
- **Pairing fails / "Rejecting pairing request":** pairing mode must be active when you click Connect. Forget the device on **both** sides (Mac and **Forget paired host**) and retry.
- **Repeated disconnects:** Wi-Fi and Bluetooth share one radio. Keep `power_save_mode: light` (do not set `none`), keep the board near the router, and avoid a busy 2.4 GHz channel. Check Wi-Fi signal in HA.
- **OTA fails:** check `ota_password` matches, keep the device on a good signal, retry. If flash is ever tight, the report shows headroom of about 600 KB per slot.
- **Logs:** over USB with `esphome logs jiggler.yaml`, or wirelessly from the Device Builder (Logs). Look for lines tagged `ble_hid_mouse`.
- **Bonds lost unexpectedly:** only a full flash erase or **Forget paired host** removes them. Normal OTA, reboots, and config changes do not.

## Customizing

All in the `substitutions:` block of `jiggler.yaml`:
- **Identity:** `ble_name` (max 20 chars), `ble_manufacturer`, `ble_model`, `ble_vid_source`, `ble_vid`, `ble_pid`, `ble_version`. After changing the name, forget the device on the Mac and re-pair.
- **Defaults:** `default_min_interval`, `default_max_interval`, `default_amplitude`, `default_display_timeout` apply on first boot only; after that HA's values persist.
- **Hardware:** `pin_sda`, `pin_scl`, `pin_led`, `led_inverted`, `pin_button`, `i2c_address`, `oled_*`.
- Settings are saved with ESPHome's rate-limited flash writes (`flash_write_interval: 5min`), so rapid tweaking in HA doesn't wear the flash. Consequence: a setting changed within 5 minutes before a power loss may not persist.
- Pairing window length: `pairing_timeout`.
- Jiggle timing constants (step and gap delays) are at the top of `components/ble_hid_mouse/ble_hid_mouse.cpp`.

## Security note

The Bluetooth name, manufacturer string and VID/PID are what macOS and any device-management software will see. The defaults imitate a Logitech mouse; that is only a label, not a guarantee of anything. Managed Macs may restrict or log Bluetooth devices, and the policies on your employer's or school's equipment are yours to follow. Nothing here makes the device invisible to IT.

## Project layout

```
jiggler.yaml                  main ESPHome config
secrets.example.yaml          placeholders for Wi-Fi, API key, OTA password
components/ble_hid_mouse/     custom component (HID-over-GATT, pairing, jiggle scheduler, OLED icon drawing in jiggler_ui.h)
ha/                           example automations (alert, schedule, restart) and dashboard card
tools/simulate_jitter.py      scheduler/jitter simulation with assertions
```
