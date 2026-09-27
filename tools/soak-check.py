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

Every counter is cumulative since boot, so the last heartbeat and its
audio line cover the whole run even when the capture garbles a line
between. The other counters are reported and must not move either, because the
hardware notes' own soak found a problem with all four clean (§12.3):
undocumented opcodes, dropped snapshots, the beeper's overflow, and log
lines dropped. Presents, key events and speaker edges must grow: a soak
that exercised nothing proves nothing.

Whether it ran on battery is in the log since M13: each heartbeat ends
with the southbridge's gauge, and "charging" there means USB power. A run
that shows charging fails, unless --usb says it was meant to be on USB.
The converse does not hold: the bit is the charger's, and clears once a
full battery on USB has finished charging (hardware-notes.md §6), so a
run that never shows it still needs the operator's word.
A log from before M13 has no gauge, and the power stays unknown.
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
BAT = re.compile(r"heartbeat\s*:.*\| battery (\?|(\d+)%( charging)?)")
DIE = re.compile(r"heartbeat\s*:.*\| battery [^,\n]*, (-?\d+) C")
BANNER = "pico-atom — Acorn Atom for the PicoCalc"
FIELD_HZ = 60


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--minutes", type=float, default=30)
    ap.add_argument("--usb", action="store_true",
                    help="the run was meant to be on USB power, not battery")
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
    # Every counter checked here is cumulative since boot (main.c's
    # heartbeat, audio_stats()), so a line the capture garbled hides
    # nothing as long as a later one was read: the last heartbeat's
    # audio line covers the run. The capture does garble the odd line,
    # a UART framing slip on the host side with the firmware's own
    # "log dropped" at zero; a missing heartbeat is sorted out below.
    last_hb = list(HB.finditer(text))[-1].start()
    if list(AU.finditer(text))[-1].start() < last_hb:
        fails.append("the last heartbeat has no audio line after it, so the "
                     "audio counters do not cover the end of the run")
    garbled_au = max(0, len(hbs) - len(aus))

    fields = [int(h[0]) for h in hbs]
    span = (fields[-1] - fields[0]) / FIELD_HZ / 60
    if span < a.minutes:
        fails.append("heartbeats span %.1f minutes, less than %g" % (span, a.minutes))
    # A heartbeat comes every 300 guest fields, not every five wall
    # seconds, so a stall cannot skip a count: it shows as rt falling.
    # A count is missing only if the firmware dropped the line, which it
    # counts as "log dropped", or the capture garbled it. With nothing
    # dropped, a missing whole number of heartbeats is the capture's.
    step = 5 * FIELD_HZ
    dropped_log = max(int(u[5]) for u in aus)
    gaps, garbled_hb = [], 0
    for x, y in zip(fields, fields[1:]):
        if y > x and (y - x) % step == 0 and dropped_log == 0:
            garbled_hb += (y - x) // step - 1
        else:
            gaps.append((x, y))
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

    bats = [m.groups() for m in BAT.finditer(text)]
    charging = sum(1 for b in bats if b[2])
    levels = [int(b[1]) for b in bats if b[1]]
    if not bats:
        power = "unknown: no battery gauge in the log (before M13)"
    elif charging:
        power = "USB: charging on %d of %d heartbeats" % (charging, len(bats))
        if not a.usb:
            fails.append("the battery was charging, so this was not on battery (--usb if meant)")
    elif levels:
        power = ("never charging, %d%% to %d%%: battery, or USB with the charge done"
                 % (levels[0], levels[-1]))
    else:
        power = "unknown: the gauge was never read"

    dies = [int(m.group(1)) for m in DIE.finditer(text)]

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
    print("  power                  %s" % power)
    if dies:
        print("  die temperature        %d to %d C, %d C at the end (uncalibrated)"
              % (min(dies), max(dies), dies[-1]))
    if garbled_au or garbled_hb:
        print("  %d heartbeat and %d audio lines garbled in the capture; the counters are "
              "cumulative, so the later ones cover them" % (garbled_hb, garbled_au))
    if fails:
        print("FAIL")
        for f in fails:
            print("  " + f)
        return 1
    print("PASS" if charging else "PASS (record whether it was on battery)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
