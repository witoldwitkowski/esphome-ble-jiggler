#!/usr/bin/env python3
"""Simulate the jiggle scheduler used by components/ble_hid_mouse/ble_hid_mouse.cpp.

This is a line-for-line model of the C++ logic (interval draw, clamping, amplitude and
direction choice, out/gap/back report sequence, skip-while-disconnected, and
undo-on-reconnect), driven by a simulated clock. It asserts the invariants from the spec:

  * every interval is within [min, max] (with min > max clamped to max = min)
  * net cursor displacement of every completed jiggle is exactly 0
  * amplitude stays within 1..configured amplitude (and the 1..5 hard range)
  * no report is sent while disconnected / unsubscribed, and a skipped jiggle is not queued
  * a jiggle cut short by a disconnect is undone on reconnect (net displacement returns to 0)
  * no report ever carries a button bit or wheel movement

It does NOT test Bluetooth; it tests the scheduling/jitter logic only.
Run:  python3 tools/simulate_jitter.py
"""

import random
import sys

MIN_INTERVAL_S, MAX_INTERVAL_S = 10.0, 3600.0
MIN_AMP, MAX_AMP = 1, 5
STEP_MS = (12, 24)
GAP_MS = (30, 120)


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


class Sim:
    def __init__(self, min_s, max_s, amp, seed):
        self.rng = random.Random(seed)
        self.min_s = clamp(float(min_s), MIN_INTERVAL_S, MAX_INTERVAL_S)
        self.max_s = clamp(float(max_s), MIN_INTERVAL_S, MAX_INTERVAL_S)
        self.amp = clamp(int(amp), MIN_AMP, MAX_AMP)
        self.warned = self.min_s > self.max_s
        self.now = 0
        self.ready = False
        self.events = []  # (time_ms, seq, fn)
        self.seq = 0
        self.reports = []  # (time_ms, buttons, dx, dy, wheel)
        self.net = [0, 0]
        self.intervals = []
        self.jiggles = []  # (amp, ux, uy)
        self.skipped = 0
        self.last_fire = 0
        self.phase = None
        self.enabled = False

    # --- scheduler core -------------------------------------------------------
    def effective_range(self):
        lo, hi = self.min_s, self.max_s
        if hi < lo:
            hi = lo  # clamp: max can never be below min
        return lo, hi

    def at(self, delay_ms, fn):
        self.seq += 1
        self.events.append((self.now + delay_ms, self.seq, fn))

    def schedule_next(self):
        lo, hi = self.effective_range()
        delay = self.rng.randint(int(lo * 1000), int(hi * 1000))
        self.intervals.append(delay)
        self.at(delay, self.on_timer)

    def enable(self):
        self.enabled = True
        self.schedule_next()

    def on_timer(self):
        if not self.enabled:
            return
        if self.ready:
            self.start_jiggle()
        else:
            self.skipped += 1  # skipped, not queued
        self.schedule_next()

    # --- report sending --------------------------------------------------------
    def send(self, dx, dy):
        if not self.ready:
            return False
        self.reports.append((self.now, 0, dx, dy, 0))
        self.net[0] += dx
        self.net[1] += dy
        return True

    def start_jiggle(self):
        if self.phase is not None:
            return
        amp = self.rng.randint(1, self.amp)
        ux, uy = self.rng.choice([(1, 0), (-1, 0), (0, 1), (0, -1)])
        self.jiggles.append((amp, ux, uy))
        self.cur = (amp, ux, uy)
        self.phase, self.remaining = "out", amp
        self.step()

    def step(self):
        amp, ux, uy = self.cur
        if self.phase == "out":
            if not self.send(ux, uy):
                self.phase = None
                return
            self.remaining -= 1
            if self.remaining == 0:
                self.phase = "gap"
                self.at(self.rng.randint(*GAP_MS), self.step)
            else:
                self.at(self.rng.randint(*STEP_MS), self.step)
        elif self.phase == "gap":
            self.phase, self.remaining = "back", amp
            self.step()
        elif self.phase == "back":
            if not self.send(-ux, -uy):
                self.phase = None
                return
            self.remaining -= 1
            if self.remaining == 0:
                self.phase = None
                self.completed.append(list(self.net))
            else:
                self.at(self.rng.randint(*STEP_MS), self.step)

    completed = None

    # --- link events -----------------------------------------------------------
    def set_ready(self, ready):
        if ready == self.ready:
            return
        self.ready = ready
        if not ready:
            self.phase = None  # abort mid-jiggle, like cancel_timeout("jig_step") in C++
            self.events = [e for e in self.events if e[2] != self.step]
        elif self.net != [0, 0]:
            # undo leftover displacement on re-subscribe
            cx, cy = -self.net[0], -self.net[1]
            if self.send(cx, cy):
                pass

    def run(self, until_ms):
        while True:
            self.events.sort(key=lambda e: (e[0], e[1]))
            if not self.events or self.events[0][0] > until_ms:
                break
            t, _, fn = self.events.pop(0)
            self.now = t
            if fn == self.step and self.phase is None:
                continue
            fn()
        self.now = until_ms


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)


def scenario_connected(seed, min_s, max_s, amp, hours=72):
    s = Sim(min_s, max_s, amp, seed)
    s.completed = []
    s.ready = True
    s.enable()
    s.run(hours * 3600 * 1000)
    while s.phase is not None:  # let an in-flight jiggle finish at the window boundary
        s.run(s.now + 50)
    lo, hi = s.effective_range()
    check(len(s.intervals) > 10, "too few intervals drawn")
    for d in s.intervals:
        check(lo * 1000 <= d <= hi * 1000, f"interval {d} outside [{lo * 1000}, {hi * 1000}]")
    for amp_used, ux, uy in s.jiggles:
        check(MIN_AMP <= amp_used <= min(s.amp, MAX_AMP), f"amplitude {amp_used} out of range")
        check(abs(ux) + abs(uy) == 1, "direction must be a single axis step")
    check(len(s.completed) == len(s.jiggles), "every jiggle must complete when connected")
    for net in s.completed:
        check(net == [0, 0], f"net displacement {net} != 0")
    check(s.net == [0, 0], f"cursor ended displaced: {s.net}")
    for _, b, dx, dy, w in s.reports:
        check(b == 0 and w == 0, "button/wheel must always be 0")
        check(abs(dx) <= MAX_AMP and abs(dy) <= MAX_AMP, "report step larger than amplitude")
    # per-jiggle gap between halves
    return s


def scenario_disconnected():
    s = Sim(30, 40, 2, 7)
    s.completed = []
    s.ready = False
    s.enable()
    s.run(30 * 60 * 1000)
    check(len(s.reports) == 0, "reports sent while disconnected")
    check(s.skipped > 0 and len(s.jiggles) == 0, "jiggles must be skipped, not queued, while disconnected")
    # connect, and confirm no backlog burst appears
    s.set_ready(True)
    before = len(s.reports)
    s.run(s.now + 1000)
    check(len(s.reports) - before <= 2 * MAX_AMP, "backlog of skipped jiggles was replayed")
    return s


def scenario_flaky_link(seed):
    s = Sim(10, 20, 5, seed)
    s.completed = []
    s.ready = True
    s.enable()
    # toggle the link at random moments, including mid-jiggle
    t = 0
    for _ in range(400):
        t += s.rng.randint(5, 25000)
        up = s.rng.random() < 0.5
        s.at(max(0, t - s.now), (lambda u=up: s.set_ready(u)))
    s.run(t + 60000)
    s.set_ready(True)
    while s.phase is not None:  # let an in-flight jiggle finish
        s.run(s.now + 50)
    check(s.net == [0, 0], f"flaky link left cursor displaced: {s.net}")
    return s


def scenario_clamp():
    s = Sim(300, 100, 2, 3)  # min > max
    check(s.warned, "min > max must raise the warning path")
    lo, hi = s.effective_range()
    check(lo == hi == 300, f"min>max must clamp to max=min (got {lo},{hi})")
    s.completed = []
    s.ready = True
    s.enable()
    s.run(3 * 3600 * 1000)
    check(all(d == 300000 for d in s.intervals), "clamped interval must equal min")
    # range clamping of configured values
    t = Sim(1, 99999, 99, 1)
    check(t.min_s == MIN_INTERVAL_S and t.max_s == MAX_INTERVAL_S and t.amp == MAX_AMP, "range clamps")


def main():
    total = 0
    for seed in range(20):
        for (mn, mx, amp) in [(120, 180, 2), (10, 10, 1), (60, 3600, 5), (10, 3600, 3)]:
            s = scenario_connected(seed, mn, mx, amp)
            total += len(s.jiggles)
    print(f"connected scenarios: OK ({total} jiggles, all net-zero, all intervals in range)")
    scenario_disconnected()
    print("disconnected scenario: OK (no reports, jiggles skipped not queued, no backlog on connect)")
    n = 0
    for seed in range(30):
        n += len(scenario_flaky_link(seed).jiggles)
    print(f"flaky-link scenarios: OK ({n} jiggles, cursor always returns to 0,0)")
    scenario_clamp()
    print("clamping scenario: OK (min > max -> max = min; values clamped to allowed ranges)")
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
