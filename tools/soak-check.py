#!/usr/bin/env python3
"""soak-check.py — hold a soak log to design.md §15.3.

    tools/soak-check.py [--minutes 30] out/soak/soak-YYYYMMDD-HHMMSS.log

Passes when the log shows one boot, heartbeats covering the whole run,
and, on every heartbeat:

  i2c errors 0, keys lost 0, underrun samples 0, late refills 0,
  rt at least 0.995, with the run's mean at least 0.999.

rt is guest seconds per wall second over a heartbeat's five seconds. The
guest is paced by the audio queue (§12.2), so it reads 1.000 or 0.999,
the last digit being truncation; a guest falling behind shows as a run of
lower figures, which the per-heartbeat floor catches.

The other counters are reported and must not move either, because the
hardware notes' own soak found a problem with all four clean (§12.3):
undocumented opcodes, dropped snapshots, the beeper's overflow, and log
lines dropped. Presents, key events and speaker edges must grow: a soak
that exercised nothing proves nothing.

Whether it ran on battery is not in the log. Say so when recording it.
"""

import argparse
import re
import sys

HB = re.compile(
    r"heartbeat\s*: (\d+) fields, rt (\d+)\.(\d+), (\d+) guest cycles, (\d+) undoc op\(s\).*?"
    r"(\d+) presents \((\d+) full, (\d+) dropped\).*?i2c errors (\d+) \| keys (\d+) \((\d+) lost\)")
AU = re.compile(
    r"audio\s*: (\d+) Hz consumed.*?underrun samples (\d+), late refills (\d+), "
    r"core overflow (\d+), speaker edges (\d+), log dropped (\d+)")
BANNER = "pico-atom — Acorn Atom for the PicoCalc"
FIELD_HZ = 60


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--minutes", type=float, default=30)
    a = ap.parse_args()

    text = open(a.log, errors="replace").read()
    hbs = [m.groups() for m in HB.finditer(text)]
    aus = [m.groups() for m in AU.finditer(text)]
    fails = []

    boots = text.count(BANNER)
    if boots != 1:
        fails.append("%d boots in the log, not 1 (a reset during the run?)" % boots)
    if not hbs or not aus:
        print("FAIL: no heartbeats in %s" % a.log)
        return 1

    fields = [int(h[0]) for h in hbs]
    span = (fields[-1] - fields[0]) / FIELD_HZ / 60
    if span < a.minutes:
        fails.append("heartbeats span %.1f minutes, less than %g" % (span, a.minutes))
    gaps = [(x, y) for x, y in zip(fields, fields[1:]) if y - x != 5 * FIELD_HZ]
    if gaps:
        fails.append("%d gaps between heartbeats, first after field %d" % (len(gaps), gaps[0][0]))

    rts = [int(h[1]) + int(h[2]) / 1000 for h in hbs]
    low = min(rts)
    mean = sum(rts) / len(rts)
    if low < 0.995:
        fails.append("rt fell to %.3f (heartbeat %d)" % (low, rts.index(low)))
    if mean < 0.999:
        fails.append("mean rt %.4f" % mean)

    def last(rows, k):
        return int(rows[-1][k])

    def worst(rows, k):
        return max(int(r[k]) for r in rows)

    listed = [
        ("i2c errors", worst(hbs, 8)),
        ("keys lost", worst(hbs, 10)),
        ("underrun samples", worst(aus, 1)),
        ("late refills", worst(aus, 2)),
    ]
    others = [
        ("undocumented opcodes", worst(hbs, 4)),
        ("dropped snapshots", worst(hbs, 7)),
        ("beeper overflow", worst(aus, 3)),
        ("log lines dropped", worst(aus, 5)),
    ]
    for name, v in listed + others:
        if v:
            fails.append("%s: %d" % (name, v))

    grew = [
        ("presents", int(hbs[0][5]), last(hbs, 5)),
        ("key events", int(hbs[0][9]), last(hbs, 9)),
        ("speaker edges", int(aus[0][4]), last(aus, 4)),
    ]
    for name, first, final in grew:
        if final <= first:
            fails.append("%s did not grow (%d to %d): not exercised" % (name, first, final))

    rates = sorted(int(u[0]) for u in aus)
    print("soak: %s" % a.log)
    print("  %d heartbeats over %.1f minutes, fields %d to %d, one boot: %s"
          % (len(hbs), span, fields[0], fields[-1], "yes" if boots == 1 else "no (%d)" % boots))
    print("  rt min %.3f, mean %.4f" % (low, mean))
    for name, v in listed:
        print("  %-22s %d" % (name, v))
    for name, v in others:
        print("  %-22s %d" % (name, v))
    for name, first, final in grew:
        print("  %-22s %d -> %d" % (name, first, final))
    print("  audio consumed         %d-%d Hz (the control quantity)" % (rates[0], rates[-1]))
    if fails:
        print("FAIL")
        for f in fails:
            print("  " + f)
        return 1
    print("PASS (record whether it was on battery)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
