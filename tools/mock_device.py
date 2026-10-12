#!/usr/bin/env python3
"""Pretend to be the jiggler so the web page can be developed and screenshotted in a browser.

    python3 tools/mock_device.py [setup|home|paired|pairing] [port]

It serves components/jiggler_portal/web/index.html and implements the same /api/* routes as
jiggler_portal.cpp, with just enough behaviour to click through every flow.
"""
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

PAGE = Path(__file__).resolve().parent.parent / "components/jiggler_portal/web/index.html"
PROFILES = [
    ("Logitech MX Anywhere 3", "MX Anywhere 3", "Logitech", "046D:B025"),
    ("Logitech MX Master 3S", "MX Master 3S", "Logitech", "046D:B034"),
    ("Logitech MX Master 3", "MX Master 3", "Logitech", "046D:B023"),
    ("Logitech MX Anywhere 2S", "MX Anywhere 2S", "Logitech", "046D:B01A"),
    ("Logitech M720 Triathlon", "M720 Triathlon", "Logitech", "046D:B015"),
    ("Microsoft Bluetooth Mouse", "Microsoft Mouse", "Microsoft", "045E:0001"),
    ("Dell Wireless Mouse", "Dell Wireless Mouse", "Dell", "413C:0001"),
    ("HP Bluetooth Mouse", "HP Bluetooth Mouse", "HP", "03F0:0001"),
    ("Lenovo Bluetooth Mouse", "Lenovo BT Mouse", "Lenovo", "17EF:0001"),
    ("Generic Bluetooth mouse", "Bluetooth Mouse", "Generic", "1209:0001"),
]
APS = [
    ("Maple Street 5G", -48, True), ("Maple Street", -52, True), ("xfinitywifi", -61, False),
    ("NETGEAR-7C", -70, True), ("Guest", -74, True), ("TP-Link_A4F2", -82, True),
]

scenario = sys.argv[1] if len(sys.argv) > 1 else "home"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
t0 = time.time()
lock = threading.Lock()
S = {
    "on": scenario in ("home", "paired"), "bond": scenario != "setup", "ready": scenario in ("home",),
    "pair_until": time.time() + 95 if scenario == "pairing" else 0,
    "up": scenario == "home" or scenario == "paired", "ap": scenario == "setup", "nets": [] if scenario == "setup" else ["Maple Street"],
    "ssid": "Maple Street", "min": 120, "max": 180, "amp": 2, "profile": 0, "cname": "", "cmfr": "", "cmodel": "",
    "dim": 60, "days": 0, "display": True, "last": time.time() - 14, "next": time.time() + 102, "count": 7,
    "trying_since": 0, "join_at": 0, "join_ssid": "", "dead_until": 0,
}


def bars(r):
    return 4 if r > -55 else 3 if r > -65 else 2 if r > -75 else 1


def state():
    now = time.time()
    left = max(0, int(S["pair_until"] - now))
    pair = left > 0
    rssi = -58
    w = {"up": S["up"], "ap": S["ap"], "saved": len(S["nets"]), "ip": "192.168.1.23" if S["up"] else "192.168.4.1",
         "host": "mouse-jiggler-a1b2c3.local"}
    if S["up"]:
        w.update(ssid=S["ssid"], rssi=rssi, bars=bars(rssi), ch=6, roam=False)
    elif S["trying_since"]:
        w["trying"] = int(now - S["trying_since"])
    prof = PROFILES[S["profile"]] if S["profile"] >= 0 else (None, S["cname"] or "Bluetooth Mouse")
    return {
        "fw": "1.0.0", "esphome": "2026.9.1", "name": "mouse-jiggler-a1b2c3", "tag": "a1b2c3",
        "up": int(now - t0) + 5400, "heap": 142000, "on": S["on"],
        "bt": {"link": S["ready"], "ready": S["ready"] and not pair, "bond": S["bond"], "pair": pair, "left": left, "idle": 0 if S["ready"] else 340},
        "j": {"next": max(0, int(S["next"] - now)) if S["on"] else -1, "prog": max(0, min(1, 1 - (S["next"] - now) / 150)),
              "count": S["count"], "ago": int(now - S["last"]), "min": S["min"], "max": S["max"], "amp": S["amp"]},
        "w": w, "nets": [{"ssid": n, "hidden": False} for n in S["nets"]],
        "id": {"profile": S["profile"], "name": prof[1], "cname": S["cname"], "cmfr": S["cmfr"], "cmodel": S["cmodel"]},
        "cfg": {"dim": S["dim"], "days": S["days"], "display": S["display"]},
    }


def tick():
    while True:
        time.sleep(0.5)
        with lock:
            now = time.time()
            if S["join_at"] and now >= S["join_at"]:
                S["join_at"] = 0
                if S["join_ssid"] == "Maple Street" or S["join_ssid"] == "Maple Street 5G":
                    S["nets"].append(S["join_ssid"]) if S["join_ssid"] not in S["nets"] else None
                    S["ssid"], S["up"], S["ap"], S["trying_since"] = S["join_ssid"], True, False, 0
                    S["dead_until"] = now + 8  # the setup network vanishes under the phone
            if S["on"] and now >= S["next"]:
                S["last"], S["count"], S["next"] = now, S["count"] + 1, now + 150


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json"):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        u = urlparse(self.path)
        if time.time() < S["dead_until"]:
            self.connection.close()
            return
        if u.path.startswith("/api/"):
            with lock:
                if u.path == "/api/state":
                    return self.send(200, json.dumps(state()))
                if u.path == "/api/scan":
                    aps = [{"ssid": s, "rssi": r, "bars": bars(r), "lock": l, "ch": 6, "saved": s in S["nets"]} for s, r, l in APS]
                    return self.send(200, json.dumps({"aps": aps}))
                if u.path == "/api/profiles":
                    return self.send(200, json.dumps([{"i": i, "label": p[0], "name": p[1], "mfr": p[2], "ids": p[3]} for i, p in enumerate(PROFILES)]))
            return self.send(404, '{"ok":false,"error":"Unknown request."}')
        self.send(200, PAGE.read_bytes(), "text/html")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        q = {k: v[0] for k, v in parse_qs(self.rfile.read(n).decode()).items()}
        p = urlparse(self.path).path[5:]
        ok = {"ok": True}
        with lock:
            if p == "enable":
                S["on"] = q.get("on") == "1"
                S["next"] = time.time() + 120
            elif p == "pair":
                S["pair_until"] = time.time() + 120
            elif p == "pair_stop":
                S["pair_until"] = 0
            elif p == "forget":
                S["bond"], S["ready"], S["pair_until"] = False, False, time.time() + 120
            elif p == "timing":
                S["min"], S["max"], S["amp"] = int(float(q["min"])), int(float(q["max"])), int(q["amp"])
            elif p == "jiggle":
                S["last"] = time.time()
            elif p == "device":
                S["dim"], S["days"], S["display"] = int(q["dim"]), int(q["days"]), q["display"] == "1"
            elif p == "profile":
                S["profile"] = -1 if q["id"] == "custom" else int(q["id"])
                if S["profile"] < 0:
                    S["cname"], S["cmfr"], S["cmodel"] = q.get("name", ""), q.get("mfr", ""), q.get("model", "")
                S["bond"], S["ready"] = False, False
            elif p == "wifi_add":
                if q.get("psk") and len(q["psk"]) < 8:
                    ok = {"ok": False, "error": "Wi-Fi passwords are 8 to 63 characters."}
                elif S["up"]:
                    S["nets"].append(q["ssid"])
                else:
                    S["join_ssid"], S["join_at"], S["trying_since"] = q["ssid"], time.time() + (4 if q.get("psk") == "goodpassword" else 99999), time.time()
                    S["nets"] = [q["ssid"]]
            elif p == "wifi_remove":
                S["nets"] = [x for x in S["nets"] if x != q.get("ssid")]
            elif p not in ("scan_start", "restart", "factory", "wifi_reset"):
                ok = {"ok": False, "error": "Unknown request."}
        self.send(200, json.dumps(ok))


if __name__ == "__main__":
    threading.Thread(target=tick, daemon=True).start()
    print(f"mock jiggler ({scenario}) on http://127.0.0.1:{port}")
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
