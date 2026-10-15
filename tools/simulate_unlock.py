#!/usr/bin/env python3
"""Replay the wake-and-unlock macro's action stream and assert its invariants.

Compiles the REAL planner header, components/ble_hid_mouse/unlock_plan.h, with
tools/unlock_harness.cpp. The BLE glue (keyboard.cpp) is not covered: that part needs hardware.

Asserted, for many passwords (every printable ASCII character included), with and without
press_return, dry run, and slow connection intervals:
  * the wiggle is four moves that sum to zero, before any key
  * keys come in order: Space, Return, the password characters, then Return only if press_return
  * the typed characters decode back to exactly the password (untypeable characters are skipped)
  * every key down is followed at once by a key up, and nothing is held at the end
  * a dry run sends no key at all
  * every move and key report waits at least the connection interval (and 30 ms)
  * cancelling at every possible point leaves no key held and starts no new key

Run:  python3 tools/simulate_unlock.py
"""
import random
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from simulate_keys import expected  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / ".esphome" / "unlock_harness"
INV = {v: k for k, v in expected().items()}
SPACE, ENTER = 0x2C, 0x28


def build():
    BIN.parent.mkdir(exist_ok=True)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-o", str(BIN),
                    str(ROOT / "tools/unlock_harness.cpp")], check=True)


def run(password="", min_iv=0, cancel_after=-1, **kw):
    args = [str(BIN), f"password={password}", f"min_iv={min_iv}", f"cancel_after={cancel_after}"]
    args += [f"{k}={int(v)}" for k, v in kw.items()]
    out = subprocess.run(args, check=True, capture_output=True).stdout.decode().strip().splitlines()
    acts = []
    for line in out:
        n, k, dx, dy, mod, usage, delay, step = line.split()
        acts.append(dict(kind=k, dx=int(dx), dy=int(dy), mod=int(mod), usage=int(usage),
                         delay=int(delay), step=int(step)))
    return acts


def typeable(pw):
    return "".join(c for c in pw if c in INV.values())


def check_full(pw, min_iv, press_return, dry):
    acts = run(pw, min_iv, press_return=press_return, dry_run=dry)
    assert acts[-1]["kind"] == "E"
    moves = [a for a in acts if a["kind"] == "M"]
    assert len(moves) == 4 and sum(a["dx"] for a in moves) == 0 and sum(a["dy"] for a in moves) == 0, "wiggle net zero"
    first_key = next((i for i, a in enumerate(acts) if a["kind"] in "DU"), len(acts))
    assert all(a["kind"] != "M" for a in acts[first_key:]), "moves only before keys"
    downs = [a for a in acts if a["kind"] == "D"]
    if dry:
        assert not downs and not any(a["kind"] == "U" for a in acts), "dry run sends no key"
    else:
        # strict D,U pairing
        keys = [a for a in acts if a["kind"] in "DU"]
        assert len(keys) % 2 == 0 and all(keys[i]["kind"] == "D" and keys[i + 1]["kind"] == "U"
                                          for i in range(0, len(keys), 2)), "down/up pairing"
        assert all(keys[i + 1]["usage"] == 0 and keys[i + 1]["mod"] == 0 for i in range(0, len(keys), 2))
        seq = [(a["mod"], a["usage"]) for a in downs]
        assert seq[0] == (0, SPACE) and seq[1] == (0, ENTER), "Space then Return first"
        body = seq[2:]
        if press_return:
            assert body and body[-1] == (0, ENTER), "final Return"
            body = body[:-1]
        decoded = "".join(INV[m_u] for m_u in body)
        assert decoded == typeable(pw), f"typed {decoded!r} != {typeable(pw)!r}"
        assert [a["step"] for a in downs[:2]] == [2, 3]
    for a in acts:
        if a["kind"] in "MDU":
            assert a["delay"] >= max(min_iv, 30), "report faster than the connection interval"
    assert all(b["step"] >= a["step"] for a, b in zip(acts, acts[1:])), "steps never go backwards"
    return len(acts)


def check_cancel(pw, dry):
    n = len(run(pw, dry_run=dry))
    for k in range(0, n + 2):
        acts = run(pw, cancel_after=k, dry_run=dry)
        assert acts[-1]["kind"] == "E"
        down = False
        for a in acts:
            if a["kind"] == "D":
                down = True
            elif a["kind"] == "U":
                down = False
        assert not down, f"cancel_after={k}: key still held"
        assert not any(a["kind"] == "D" for a in acts[k:]), f"cancel_after={k}: key pressed after cancel"
    return n + 2


def main():
    build()
    rng = random.Random(11)
    allprint = "".join(chr(c) for c in range(0x20, 0x7F))
    pws = ["hunter2", "Pa55 w0rd!", "x", allprint, "péssörd1", "A" * 60, "!@#$%^&*()_+{}|:\"<>?~"]
    pws += ["".join(rng.choice(allprint) for _ in range(rng.randint(1, 40))) for _ in range(40)]
    runs = 0
    for pw in pws:
        for miv in (0, 15, 30, 50):
            for pr in (0, 1):
                for dry in (0, 1):
                    check_full(pw, miv, pr, dry)
                    runs += 1
    cancels = 0
    for pw in ("hunter2", "Pa55 w0rd!"):
        for dry in (0, 1):
            cancels += check_cancel(pw, dry)
    print(f"unlock planner: {runs} runs and {cancels} cancel points OK")
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as e:
        print("FAIL:", e)
        sys.exit(1)
