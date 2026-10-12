#!/usr/bin/env python3
"""Replay the sleep macro's HID report stream and assert its invariants.

Unlike simulate_jitter.py (which re-implements the scheduler in Python), this compiles the REAL
planner header, components/ble_hid_mouse/sleep_macro_plan.h, with tools/macro_harness.cpp and
checks the report sequence it produces. The BLE and ESPHome glue (sleep_macro.cpp) is not
covered: that part needs hardware.

Asserted for the full macro and each calibration test, top-left and top-right, many random
parameter sets:
  * X is driven fully to the corner side before any Y slam report; Y is then driven negative
  * the drift sums equal the configured counts times the scale (X mirrored for top-right)
  * clicks: FULL = exactly 2 press/release pairs, dry run = 0, tests 1/2 = 0, tests 3/4 = 1
    (test 3 and 4 click the Apple logo to open the menu; neither ever clicks Sleep)
  * a button press never carries movement, and every press is followed by a release
  * cancelling at any point releases a held button and sends no further press
  * pacing: every moving report waits at least the connection interval
  * the pointer model ends where expected (corner, logo, or Sleep row)
  * the Tahoe preset in the C++ table equals the initial values in jiggler.yaml

Run:  python3 tools/simulate_macro.py
"""
import random
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / ".esphome" / "macro_harness"   # .esphome/ is gitignored

DEFAULTS = dict(slam_count=40, slam_cpr=127, slam_interval_ms=15, settle_ms=500, slam_passes=1,
                drift_cps=2, drift_interval_ms=20, drift_scale_pct=100, logo_x=24, logo_y=14,
                sleep_y=190, sleep_x=0, dwell_before_ms=300, click_hold_ms=80, menu_dwell_ms=800,
                hover_dwell_ms=400, confirm_timeout_s=30, top_right=0, dry_run=0)


def build():
    BIN.parent.mkdir(exist_ok=True)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-o", str(BIN),
                    str(ROOT / "tools/macro_harness.cpp")], check=True)


def run(mode, min_iv=0, cancel_after=-1, **kw):
    p = dict(DEFAULTS)
    p.update(kw)
    args = [str(BIN), f"mode={mode}", f"min_iv={min_iv}", f"cancel_after={cancel_after}"]
    args += [f"{k}={int(v)}" for k, v in p.items()]
    out = subprocess.run(args, check=True, capture_output=True, text=True).stdout.strip().splitlines()
    acts = []
    for line in out:
        t, kind, b, dx, dy, stage, delay = line.split()
        acts.append(dict(t=int(t), kind=kind, b=int(b), dx=int(dx), dy=int(dy), stage=int(stage), delay=int(delay)))
    return p, acts


W, H = 3456, 900          # model desktop: two 1728-wide displays side by side, top-aligned
CLAMP = lambda v, lo, hi: max(lo, min(hi, v))


def check_run(mode, p, acts, min_iv):
    tr = bool(p["top_right"])
    dry = bool(p["dry_run"])
    assert acts[-1]["kind"] == "D", "must end with DONE"
    reps = [a for a in acts if a["kind"] == "R"]
    sc = p["drift_scale_pct"] / 100.0
    rnd = lambda v: int(round(v * sc + 1e-9)) if v >= 0 else -int(round(-v * sc + 1e-9))
    sgn = -1 if tr else 1

    # --- slam ordering -------------------------------------------------------------------
    slam = [a for a in reps if a["stage"] == 1]
    first_y = next(i for i, a in enumerate(slam) if a["dy"] != 0)
    assert all(a["dx"] != 0 and a["dy"] == 0 for a in slam[:first_y]), "X must be driven first"
    assert first_y == p["slam_count"], "exactly slam_count X reports before the first Y report"
    xs = [a for a in slam[:first_y]]
    assert all((a["dx"] > 0) == tr and abs(a["dx"]) == p["slam_cpr"] for a in xs), "X direction/size"
    pass1_y = slam[first_y:first_y + p["slam_count"]]
    assert all(a["dy"] == -p["slam_cpr"] and a["dx"] == 0 for a in pass1_y), "Y negative after X"
    assert len(slam) == 2 * p["slam_count"] * p["slam_passes"]
    assert all(abs(a["dx"]) <= 127 and abs(a["dy"]) <= 127 for a in reps)

    # --- drift sums ----------------------------------------------------------------------
    d3 = [a for a in reps if a["stage"] == 3]
    d5 = [a for a in reps if a["stage"] == 5]
    if mode != 1:
        assert sum(a["dx"] for a in d3) == sgn * rnd(p["logo_x"]), "logo X sum"
        assert sum(a["dy"] for a in d3) == rnd(p["logo_y"]), "logo Y sum"
        assert all(abs(a["dx"]) <= p["drift_cps"] and abs(a["dy"]) <= p["drift_cps"] for a in d3)
    else:
        assert not d3
    if mode in (0, 4):
        assert sum(a["dx"] for a in d5) == sgn * rnd(p["sleep_x"]), "sleep X sum"
        assert sum(a["dy"] for a in d5) == rnd(p["sleep_y"]), "sleep Y sum"
        assert all(abs(a["dx"]) <= p["drift_cps"] and abs(a["dy"]) <= p["drift_cps"] for a in d5)
    else:
        assert not d5

    # --- clicks --------------------------------------------------------------------------
    presses = [a for a in reps if a["b"] != 0]
    expect = 0 if dry else {0: 2, 1: 0, 2: 0, 3: 1, 4: 1}[mode]
    assert len(presses) == expect, f"press count {len(presses)} != {expect}"
    for a in presses:
        assert a["dx"] == 0 and a["dy"] == 0, "a press must not move"
        assert a["b"] == 1, "left button only"
    held = False
    for a in reps:
        if a["b"]:
            assert not held, "press while already held"
            held = True
        elif held:
            assert a["dx"] == 0 and a["dy"] == 0
            held = False
    assert not held, "button left held at the end"
    if mode in (3, 4):
        assert all(a["stage"] == 4 for a in presses), "tests 3/4 click only the Apple logo"
    if mode == 0 and not dry:
        assert [a["stage"] for a in presses] == [4, 6]
        assert acts[-2]["kind"] == "C" and acts[-2]["delay"] == p["confirm_timeout_s"] * 1000
    else:
        assert not any(a["kind"] == "C" for a in acts), "no confirm stage outside a live full run"

    # --- pacing --------------------------------------------------------------------------
    for a in reps:
        if a["b"] == 0 and (a["dx"] or a["dy"]):
            assert a["delay"] >= min_iv, "step faster than the connection interval"
    assert all(a["stage"] >= b["stage"] for b, a in zip(acts, acts[1:])), "stages never go backwards"

    # --- pointer model -------------------------------------------------------------------
    x, y = random.randint(0, W), random.randint(0, H)
    stage_pos = {}
    for a in reps:
        x = CLAMP(x + a["dx"], 0, W)
        y = CLAMP(y + a["dy"], 0, H)
        stage_pos[a["stage"]] = (x, y)
    cx = W if tr else 0
    assert stage_pos[1] == (cx, 0), f"slam must land in the corner, got {stage_pos[1]}"
    # Each drift moves one way per axis, so its landing point is the clamped sum.
    lx = CLAMP(cx + sgn * rnd(p["logo_x"]), 0, W)
    ly = CLAMP(rnd(p["logo_y"]), 0, H)
    if mode != 1:
        assert stage_pos[3] == (lx, ly), "logo landing"
    if mode in (0, 4):
        assert stage_pos[5] == (CLAMP(lx + sgn * rnd(p["sleep_x"]), 0, W), CLAMP(ly + rnd(p["sleep_y"]), 0, H)), \
            "Sleep row landing"


def check_cancel(mode, p, n_actions):
    """Cancel at every possible point: the button must end released and no new press may follow."""
    for k in range(0, n_actions + 2):
        _, acts = run(mode, cancel_after=k, **{kk: vv for kk, vv in p.items()})
        reps = [a for a in acts if a["kind"] == "R"]
        assert acts[-1]["kind"] == "D"
        held = False
        for i, a in enumerate(reps):
            if a["b"]:
                held = True
            elif held:
                held = False
        assert not held, f"cancel_after={k}: button still held"
        # Nothing after the cancel point may press. The cancel takes effect at action index k.
        assert not any(a["b"] for a in acts[k:] if a["kind"] == "R"), f"cancel_after={k}: press after cancel"


def check_yaml_presets():
    hdr = (ROOT / "components/ble_hid_mouse/sleep_macro_plan.h").read_text()
    row = re.search(r"/\* macOS 26 Tahoe\s*\*/\s*\{([^}]*)\}", hdr).group(1)
    vals = [int(v) for v in row.split(",")]
    yaml = (ROOT / "jiggler.yaml").read_text()
    ids = ["slam_count", "slam_cpr", "slam_interval_ms", "settle_ms", "slam_passes", "drift_cps",
           "drift_interval_ms", "drift_scale_pct", "logo_x", "logo_y", "sleep_y", "sleep_x",
           "dwell_before_ms", "click_hold_ms", "menu_dwell_ms", "hover_dwell_ms"]
    assert len(vals) == len(ids)
    for k, v in zip(ids, vals):
        m = re.search(rf"id: m_{k}\n(?:.*\n)*?    initial_value: (-?\d+)", yaml)
        assert m and int(m.group(1)) == v, f"jiggler.yaml initial value for m_{k} differs from the preset table"


def main():
    build()
    check_yaml_presets()
    rng = random.Random(7)
    runs = 0
    # Defaults, every mode, both corners, dry run, with and without a slow connection interval.
    for mode in range(5):
        for tr in (0, 1):
            for dry in (0, 1):
                for miv in (0, 15, 30, 50):
                    p, acts = run(mode, min_iv=miv, top_right=tr, dry_run=dry)
                    check_run(mode, p, acts, miv)
                    runs += 1
    # Random parameter sets.
    for _ in range(250):
        kw = dict(slam_count=rng.randint(5, 300), slam_cpr=rng.randint(4, 127),
                  slam_interval_ms=rng.randint(5, 200), settle_ms=rng.randint(100, 5000),
                  slam_passes=rng.randint(1, 3), drift_cps=rng.randint(1, 3),
                  drift_interval_ms=rng.randint(5, 200), drift_scale_pct=rng.randint(25, 400),
                  logo_x=rng.randint(0, 300), logo_y=rng.randint(0, 200), sleep_y=rng.randint(0, 800),
                  sleep_x=rng.randint(-200, 200), top_right=rng.randint(0, 1), dry_run=rng.randint(0, 1))
        # keep the slam long enough to cross the model desktop (real Macs are the unverified part)
        kw["slam_count"] = min(1000, max(kw["slam_count"], -(-W // kw["slam_cpr"]) + 1))
        mode = rng.randint(0, 4)
        miv = rng.choice([0, 8, 15, 30, 50])
        p, acts = run(mode, min_iv=miv, **kw)
        check_run(mode, p, acts, miv)
        runs += 1
    # Cancel at every point of the full macro and of each test, live and dry.
    cancels = 0
    for mode in range(5):
        for dry in (0, 1):
            p, acts = run(mode, dry_run=dry, drift_cps=3, logo_x=9, logo_y=6, sleep_y=30, slam_count=6)
            check_cancel(mode, p, len(acts))
            cancels += len(acts) + 2
    print(f"macro planner: {runs} runs and {cancels} cancel points OK")
    # Defaults sanity: the default slam is long enough for the model desktop.
    assert 40 * 127 >= W, "default slam shorter than the model desktop"
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as e:
        print("FAIL:", e)
        sys.exit(1)
