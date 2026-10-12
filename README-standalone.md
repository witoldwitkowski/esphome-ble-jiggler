# Mouse Jiggler, standalone edition (no Home Assistant)

Same ESP32-C3 + 0.42" OLED board and the same Bluetooth LE mouse as the Home Assistant version, but everything is set up and controlled from a web page on your phone. Nothing to edit, no router changes, no extra software.

`jiggler-standalone.yaml` is the firmware. The page itself lives in `components/jiggler_portal/web/index.html`.

## What you get

- **Its own setup Wi-Fi on first boot.** Join `mouse-jiggler-xxxxxx` from your phone; the setup page opens by itself (or browse to `http://192.168.4.1`).
- **Wi-Fi search and join.** The page lists the networks the jiggler can hear, you tap one and type the password.
- **Remembers up to 5 networks** in the device's flash. It joins the strongest one it can hear.
- **Roams between access points** of the same network on its own (see below).
- **Pick which mouse it pretends to be**: ten presets (Logitech MX Anywhere 3, MX Master 3S and others) or your own name.
- **Pair, switch on, set the timing, forget the computer**, all from the page, from any device on your Wi-Fi.
- **Works with no Wi-Fi at all.** Pairing, jiggling and the button work regardless; Wi-Fi only gets you the page.
- **Firmware updates from the browser** (Device, Update firmware).

## First-time setup

1. **Flash it once over USB.** Plug the board into a computer, open <https://web.esphome.io> in Chrome or Edge, click Connect, then Install, and choose the file `firmware.factory.bin` (build it yourself, see below, or take it from a release if one is published).
   If it won't start the upload: hold BOOT, tap RST, release BOOT.
2. **Join the setup network** on your phone: `mouse-jiggler-` plus 6 characters (the same 6 are shown on the OLED, with a broadcasting symbol). It's open, no password. The page opens by itself; if not, open `http://192.168.4.1`.
3. **Choose your Wi-Fi.** The list of networks opens first. Tap yours, type the password, Join.
4. **Switch your phone back to your home Wi-Fi** and open `http://mouse-jiggler-xxxxxx.local`. The address is also on the OLED for 20 seconds after it connects, or any time you tap the BOOT button. Bookmark it.
   (Some Android versions can't open `.local` addresses. Use the numeric address from the OLED instead.)
5. **Pair.** On the page tap Pair with a computer, then on the Mac: System Settings, Bluetooth, Connect next to the name shown. On Windows: Settings, Bluetooth & devices, Add device.
6. **Switch Keep awake on.** Done. It stays set across power cuts.

You can skip step 3 entirely if you never want the page: pair with the button instead (hold BOOT 3 s) and the jiggler runs with its last settings.

## The button (BOOT)

| Press | What happens |
|---|---|
| Tap | Wake the screen and show the network address for 8 s |
| Hold 3 s | Pairing mode |
| Hold 10 s | Forget the paired computer, then pairing mode |
| Hold 20 s | Forget all Wi-Fi networks and restart into setup mode |

## Wi-Fi behavior

- **Nothing saved:** the setup network is offered immediately.
- **Saved networks but none reachable for 45 seconds** (you moved the jiggler, the router is off): the setup network comes back so you can add another, and goes away again as soon as a saved network connects. It keeps trying the saved networks the whole time.
- **Several saved networks:** it scans and joins the strongest one it knows.
- **Roaming between access points** (mesh or several routers sharing one network name): ESPHome's built-in roaming checks every 5 minutes after connecting, and only moves when another access point is at least 10 dB stronger, so it doesn't bounce around. It won't scan while a nudge is being sent.
- **Stuck on a weak signal:** ESPHome stops looking after 3 checks per connection. If the signal stays worse than -78 dBm for 10 minutes the jiggler drops the link once (at most every 30 minutes, never during a nudge) so the normal reconnect logic scans again and prefers a different access point. Bluetooth keeps running through it.
- **Hidden networks** are supported (tick the box under Other network). Networks that need a login username (company WPA2-Enterprise) are not.

## Choosing the mouse

Under Shows up as, pick a preset or Something else. Applying restarts the jiggler and forgets the paired computer, because the old pairing no longer matches. Remove the old entry from the computer's Bluetooth list, then pair again.

The Logitech vendor/product IDs come from public USB ID lists. The Microsoft, Dell, HP, Lenovo and Generic entries use the real maker's vendor ID with a generic product ID, so they only change the name and maker the computer shows. None of this was checked against a real computer yet.

## Build it yourself

```
pip install esphome            # 2026.9 or newer
esphome compile jiggler-standalone.yaml
```

The factory image ends up in `.esphome/build/mouse-jiggler/build/firmware.factory.bin`. The YAML fetches the two components from GitHub (`external_components`), so you need the repo pushed to the address in that block, or switch the block to the commented-out local one.

## What was verified, and what wasn't

| Item | Status |
|---|---|
| `esphome config` and `esphome compile` | Pass, no warnings. Flash 69.9% of one OTA slot, RAM 42.6% |
| Web page in Chrome at phone width, against a mock of the device API (`tools/mock_device.py`): first-run list, join with a bad and a good password, pairing, forgetting, switch, mouse change, timing | Works |
| Setup network appears, phone opens the page by itself, page loads from the real firmware | **Not verified** (no hardware) |
| Joining a real router, reconnecting after a power cut, several saved networks | **Not verified** |
| Roaming between two access points, weak-signal recovery | **Not verified** (logic is ESPHome's, plus one small watchdog) |
| Pairing with a Mac or Windows PC under the new presets | **Not verified** |
| OLED setup screen, address screen | **Not verified** (drawn from primitives, never seen on the panel) |
| iPhone and Android "sign in to Wi-Fi" popup behavior | **Not verified** |

## Things to know

- **The setup network is open by default**, so someone nearby could join it while it's up and change settings. It only exists while no saved Wi-Fi works. Set `setup_password` in the YAML (8 or more characters) if that matters to you.
- **The page on your home network has no login.** Anyone on your Wi-Fi can open it. Fine for a desk gadget; don't expose port 80 to the internet.
- Wi-Fi passwords are stored in the device's flash, unencrypted, like any ESPHome device that saves a network at runtime.
- Recovery if you ever lock yourself out: hold BOOT 20 s (forgets Wi-Fi), or the page's Erase all settings.
- Names and IDs are labels. Managed computers can still log and restrict Bluetooth devices, and your employer's or school's rules are yours to follow.

## Layout

```
jiggler-standalone.yaml                 the firmware config
components/jiggler_portal/              page, API, saved settings and networks
components/jiggler_portal/web/index.html  the page (one file, no internet needed)
components/ble_hid_mouse/               Bluetooth mouse, shared with the Home Assistant version
tools/mock_device.py                    fake device for working on the page in a browser
```
