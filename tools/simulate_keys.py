#!/usr/bin/env python3
"""Check the keyboard side without hardware.

  * compiles the REAL components/ble_hid_mouse/hid_keys.h and checks the ASCII to HID table against
    an independent US-layout table (every printable character, shift state included)
  * parses the REAL HID report descriptor out of ble_hid_mouse.cpp and checks that collections are
    balanced and that each Report ID has the byte size the firmware sends (mouse 4, keyboard 8 in and
    1 out, consumer 2), and that the GATT report references match those IDs

A descriptor mistake fails silently on a Mac (the device just does not work), so this is worth having.

Run:  python3 tools/simulate_keys.py
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / ".esphome" / "keymap_harness"
CPP = (ROOT / "components/ble_hid_mouse/ble_hid_mouse.cpp").read_text()

US_PLAIN = "abcdefghijklmnopqrstuvwxyz1234567890"
US_PLAIN_USAGE = list(range(0x04, 0x1E)) + list(range(0x1E, 0x28))
SYM = {  # unshifted, shifted, usage
    "-": ("_", 0x2D), "=": ("+", 0x2E), "[": ("{", 0x2F), "]": ("}", 0x30), "\\": ("|", 0x31),
    ";": (":", 0x33), "'": ('"', 0x34), "`": ("~", 0x35), ",": ("<", 0x36), ".": (">", 0x37), "/": ("?", 0x38),
}
SHIFT_DIGITS = ")!@#$%^&*("


def expected():
    exp = {}
    for ch, u in zip(US_PLAIN, US_PLAIN_USAGE):
        exp[ch] = (0, u)
        if ch.isalpha():
            exp[ch.upper()] = (2, u)
    for i, ch in enumerate(SHIFT_DIGITS):
        exp[ch] = (2, 0x27 if i == 0 else 0x1E + i - 1)
    exp[" "] = (0, 0x2C)
    for plain, (shifted, u) in SYM.items():
        exp[plain] = (0, u)
        exp[shifted] = (2, u)
    return exp


def check_keymap():
    BIN.parent.mkdir(exist_ok=True)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-o", str(BIN),
                    str(ROOT / "tools/keymap_harness.cpp")], check=True)
    out = subprocess.run([str(BIN)], check=True, capture_output=True, text=True).stdout.split("\n")
    got = {}
    for line in out:
        if line:
            c, m, k = map(int, line.split())
            got[chr(c)] = (m, k)
    exp = expected()
    printable = {chr(c) for c in range(0x20, 0x7F)}
    assert set(got) == printable, f"table must cover exactly printable ASCII; diff {set(got) ^ printable}"
    assert set(exp) == printable, f"checker table incomplete: {printable - set(exp)}"
    for ch in printable:
        assert got[ch] == exp[ch], f"{ch!r}: got {got[ch]}, expected {exp[ch]}"
    return len(printable)


def report_map():
    body = re.search(r"REPORT_MAP\[\] = \{(.*?)\n\};", CPP, re.S).group(1)
    body = re.sub(r"//[^\n]*", "", body)
    return bytes(int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", body))


def parse(desc):
    """Minimal HID item parser: returns {(report_id, 'in'/'out'): bits} and checks collection balance."""
    i, depth = 0, 0
    rid, size, count = 0, 0, 0
    sizes = {}
    while i < len(desc):
        b = desc[i]
        n = {0: 0, 1: 1, 2: 2, 3: 4}[b & 3]
        tag, typ = b >> 4, (b >> 2) & 3
        val = int.from_bytes(desc[i + 1:i + 1 + n], "little")
        assert i + 1 + n <= len(desc), "descriptor truncated"
        if typ == 1:  # global
            if tag == 7: size = val
            elif tag == 9: count = val
            elif tag == 8: rid = val
        elif typ == 0:  # main
            if tag == 0xA: depth += 1
            elif tag == 0xC:
                depth -= 1
                assert depth >= 0, "unbalanced End Collection"
            elif tag in (8, 9):  # input / output
                key = (rid, "in" if tag == 8 else "out")
                sizes[key] = sizes.get(key, 0) + size * count
        i += 1 + n
    assert depth == 0, "unbalanced collections"
    return sizes


def check_descriptor():
    sizes = parse(report_map())
    want = {(1, "in"): 4 * 8, (2, "in"): 8 * 8, (2, "out"): 8, (3, "in"): 16}
    assert sizes == want, f"report sizes {sizes} != {want}"
    # GATT report references must name exactly these IDs/types.
    refs = {n: tuple(int(v, 16) for v in re.search(rf"{n}\[\] = \{{(0x..), (0x..)\}}", CPP).groups())
            for n in ("REPORT_REF_MOUSE_IN", "REPORT_REF_KBD_IN", "REPORT_REF_KBD_OUT", "REPORT_REF_CONSUMER_IN")}
    assert refs == {"REPORT_REF_MOUSE_IN": (1, 1), "REPORT_REF_KBD_IN": (2, 1),
                    "REPORT_REF_KBD_OUT": (2, 2), "REPORT_REF_CONSUMER_IN": (3, 1)}, refs
    # Payload sizes sent by the firmware.
    kb = (ROOT / "components/ble_hid_mouse/keyboard.cpp").read_text()
    assert "uint8_t report[8] = {modifiers" in kb and "uint8_t report[2]" in kb
    assert "uint8_t report[4] = {0x00, (uint8_t) dx" in CPP
    return len(report_map())


def main():
    n = check_keymap()
    d = check_descriptor()
    print(f"keymap: {n} printable characters match the US layout table")
    print(f"descriptor: {d} bytes, collections balanced, report sizes 4/8+1/2 and report references match")
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as e:
        print("FAIL:", e)
        sys.exit(1)
