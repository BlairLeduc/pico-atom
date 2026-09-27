#!/usr/bin/env python3
"""trace-diff.py — §15.1's trace-diff harness (design.md).

Runs the same ROMs and the same keys under pico-atom's core (atom-trace)
and under Atomulator (atomulator-trace), and diffs the per-instruction
PC/A/X/Y/S/P/cycles traces:

    tools/trace-diff.py run [--keys 'PRINT 2+2\\n'] [--fields N]
    tools/trace-diff.py diff OURS.trace REF.trace

`run` finds the four ROMs in roms/ (or $PICO_ATOM_ROMS) by SHA-1, as the
tests do, and writes out/trace/{ours,ref}.trace and keys.txt. Build the
two tracers first:

    cmake --build build/host --target atom-trace
    tools/trace/build-atomulator.sh ~/path/to/Atomulator

The two machines do not keep the same time. Atomulator runs 64 cycles a
line, 16,768 a field, against the VDG's 16,667 (§12.1), and its port C
2.4 kHz bit has its own phase, so a loop that polls FS, the VIA or port C
runs a different number of times on each side. The diff resyncs across
those: at a divergence it finds the nearest point where both traces agree
again, in registers and in the return address of the call they are in,
for --confirm lines or until a timed input tells them apart again, and
says what lay between:

  reset     S after reset, which Atomulator leaves at 0. Expected.
  input     Right after a read of a timed input: the keyboard (#B001),
            FS, REPT and 2.4 kHz (#B002), the VIA, the 8271. Expected;
            counted by address.
  interrupt One side took an interrupt the other did not yet. Expected.
  loop      What each side ran alone was a loop: every PC in it either ran
            just before the divergence or repeats within it. Expected.
  values    The same PCs on both sides but some register differed.
  path      Either side ran code the other did not. The first one is
            almost always the bug.

Instruction timing is checked where the traces agree: each instruction's
cycles are the next line's count less its own, and ATOMULATOR_ERRATA
lists where Atomulator is known wrong. Exit status is 0 when there is no
`values` or `path` divergence, no other cycle difference, and the traces
resync to the end; 1 otherwise.
"""

import argparse
import hashlib
import os
import subprocess
import sys
from array import array
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# romset.c's digests; the order is the tracers' argument order.
ROMS = [
    ("kernel", "2621f27d652d4673e0a79aa669e729b8c3051ab6"),
    ("basic", "a8ea19f10d4c98fbc1b666e5968f06d46af9a84c"),
    ("float", "ebcde5b36cb3a3344567cbba4c7b9fde015f4802"),
    ("dos", "71ea0a4b8d9c3caf9718fc7cc279f4306a23b39c"),
]

# The Atom's matrix as (port A column, port B bit), per the kernel ROM's
# scan (keymap_picocalc.c, design.md §16). Shifted characters are the
# key under them with SHIFT.
CELLS = {
    "3": (0, 1), "-": (0, 2), "G": (0, 3), "Q": (0, 4), "\x1b": (0, 5),
    "2": (1, 1), ",": (1, 2), "F": (1, 3), "P": (1, 4), "Z": (1, 5),
    "1": (2, 1), ";": (2, 2), "E": (2, 3), "O": (2, 4), "Y": (2, 5),
    "0": (3, 1), ":": (3, 2), "D": (3, 3), "N": (3, 4), "X": (3, 5),
    "9": (4, 2), "C": (4, 3), "M": (4, 4), "W": (4, 5),
    "8": (5, 2), "B": (5, 3), "L": (5, 4), "V": (5, 5),
    "]": (6, 0), "\n": (6, 1), "7": (6, 2), "A": (6, 3), "K": (6, 4), "U": (6, 5),
    "\\": (7, 0), "6": (7, 2), "^": (7, 3), "J": (7, 4), "T": (7, 5),
    "[": (8, 0), "5": (8, 2), "/": (8, 3), "I": (8, 4), "S": (8, 5),
    " ": (9, 0), "4": (9, 2), ".": (9, 3), "H": (9, 4), "R": (9, 5),
}
SHIFTED = {"!": "1", '"': "2", "#": "3", "$": "4", "%": "5", "&": "6", "'": "7",
           "(": "8", ")": "9", "=": "-", "+": ";", "*": ":", "<": ",", ">": ".",
           "?": "/", "|": "\\"}

# Where Atomulator's instruction timing is known to be wrong: opcode ->
# (a 6502's cycles, Atomulator's). Found by this harness and then read off
# Atomulator's 6502.c; the 6502's figures are the MCS6500 manual's, which
# test_m6502_cycles holds the core to by execution. A difference that is
# exactly one of these is counted but is not a failure.
ATOMULATOR_ERRATA = {}
for _op in (0x15, 0x35, 0x55, 0xB4, 0xB5, 0xB6, 0xD5, 0xF5):   # zero page,X/Y read
    ATOMULATOR_ERRATA[_op] = (4, 3)
for _op in (0x16, 0x36, 0x56, 0x76, 0xD6, 0xF6):               # zero page,X RMW
    ATOMULATOR_ERRATA[_op] = (6, 5)
ATOMULATOR_ERRATA[0xDE] = ATOMULATOR_ERRATA[0xFE] = (7, 6)     # DEC/INC abs,X
ATOMULATOR_ERRATA[0xE0] = (2, 3)                               # CPX #
ATOMULATOR_ERRATA[0xEC] = (4, 3)                               # CPX abs
ATOMULATOR_ERRATA[0xA8] = (2, 0)                               # TAY

TAIL_CYCLES = 2 * 16768  # two of Atomulator's fields (Diff.run)
BOOT_FIELDS = 120   # guest_boot's two seconds to the prompt
KEY_HOLD = 3        # fields a key is down
KEY_EVERY = 8       # fields from one key to the next; the MOS takes ~8
LINE_EVERY = 60     # after RETURN: the Atom has no type-ahead, and a key
                    # pressed while a command runs is lost


def keyscript(text):
    lines = ["# written by trace-diff.py from %r" % text]
    field = BOOT_FIELDS
    for ch in text:
        up = ch.upper()
        if up in CELLS:
            col, bit = CELLS[up]
            lines.append("%d %d %d %d" % (field, KEY_HOLD, col, bit))
        elif ch in SHIFTED:
            col, bit = CELLS[SHIFTED[ch]]
            lines.append("%d %d shift" % (field, KEY_HOLD))
            lines.append("%d %d %d %d" % (field, KEY_HOLD, col, bit))
        else:
            sys.exit("trace-diff: no Atom key for %r" % ch)
        field += LINE_EVERY if ch == "\n" else KEY_EVERY
    return "\n".join(lines) + "\n", field


def find_roms():
    d = Path(os.environ.get("PICO_ATOM_ROMS", ROOT / "roms"))
    by_sha = {}
    for p in sorted(d.iterdir()) if d.is_dir() else []:
        if p.is_file() and p.stat().st_size == 4096:
            by_sha.setdefault(hashlib.sha1(p.read_bytes()).hexdigest(), p)
    missing = [n for n, s in ROMS if s not in by_sha]
    if missing:
        sys.exit("trace-diff: no %s ROM in %s (README.md lists the images)" % (", ".join(missing), d))
    return [str(by_sha[s]) for _, s in ROMS]


def load(path):
    """The trace as three arrays: the state packed into one int, the cycle
    count, and the three bytes at PC as one int, opcode first."""
    state, cyc, ops = array("Q"), array("q"), array("L")
    with open(path) as f:
        for line in f:
            w = line.split()
            if len(w) != 8:
                continue
            state.append(int(w[0], 16) << 40 | int(w[1], 16) << 32 | int(w[2], 16) << 24 |
                         int(w[3], 16) << 16 | int(w[4], 16) << 8 | int(w[5], 16))
            cyc.append(int(w[6]))
            ops.append(int(w[7], 16))
    return state, cyc, ops


def contexts(state, ops):
    """For each line, the return address of the innermost call it is in:
    a shadow of the return stack, kept from JSR, RTS/RTI and interrupt
    entry by S alone. Two lines with the same registers in a routine
    called from two places differ here."""
    ctx = array("L", [0]) * len(state)
    stack = []              # (the slot the return address's high byte is in, return address)
    for k in range(len(state)):
        sp = (state[k] >> 8) & 0xFF
        while stack and stack[-1][0] <= sp:
            stack.pop()
        if k > 0:
            prev = state[k - 1]
            psp = (prev >> 8) & 0xFF
            if ops[k - 1] >> 16 == 0x20:                 # JSR pushed PC+2
                stack.append((psp, ((prev >> 40) + 3) & 0xFFFF))
            elif (psp - 3) & 0xFF == sp and state[k] & 0x04 and not prev & 0x04:
                stack.append((psp, 0x10000 | (prev >> 40)))    # as interrupted()
        ctx[k] = stack[-1][1] if stack else 0
    return ctx


def fmt(k):
    return "%04X A=%02X X=%02X Y=%02X S=%02X P=%02X" % (
        k >> 40, (k >> 32) & 255, (k >> 24) & 255, (k >> 16) & 255, (k >> 8) & 255, k & 255)


# Reads that take an absolute address, plain or indexed: (index register,
# as its shift in the packed state) or None. Indirect reads are not
# followed; the pointer is in RAM the trace does not carry.
ABS_READS = {}
for _op in (0xAD, 0xAE, 0xAC, 0x2C, 0xCD, 0xEC, 0xCC, 0x2D, 0x0D, 0x4D, 0x6D, 0xED,
            0xEE, 0xCE, 0x0E, 0x2E, 0x4E, 0x6E):
    ABS_READS[_op] = None
for _op in (0xBD, 0xBC, 0xDD, 0x3D, 0x1D, 0x5D, 0x7D, 0xFD, 0xFE, 0xDE, 0x1E, 0x3E, 0x5E, 0x7E):
    ABS_READS[_op] = 24     # X
for _op in (0xB9, 0xBE, 0xD9, 0x39, 0x19, 0x59, 0x79, 0xF9):
    ABS_READS[_op] = 16     # Y


def timed(addr):
    """An input whose value depends on when it is read, which the two
    machines do not agree on: the keyboard rows (port B), port C's FS,
    REPT and 2.4 kHz bits, the VIA's timers and flags, and the 8271's
    status (design.md §2.3, §7.3, §7.4). Port A and port C's output
    latch read back what was written; a divergence after them is a bug."""
    return addr in (0xB001, 0xB002) or 0xB800 <= addr <= 0xBBFF or 0x0A00 <= addr <= 0x0AFF


def io_read(bytes3, state):
    """The timed input an instruction reads (timed()), or None."""
    op = bytes3 >> 16
    if op not in ABS_READS:
        return None
    addr = ((bytes3 & 0xFF) << 8) | ((bytes3 >> 8) & 0xFF)
    if ABS_READS[op] is not None:
        addr = (addr + ((state >> ABS_READS[op]) & 0xFF)) & 0xFFFF
    return addr if timed(addr) else None


def interrupted(state, lo, n):
    """The stretch begins with an interrupt entry: three bytes pushed and
    I set, by something that is not an instruction."""
    if n == 0 or lo == 0:
        return False
    a, b = state[lo - 1], state[lo]
    return ((a >> 8) & 0xFF) - 3 & 0xFF == (b >> 8) & 0xFF and b & 0x04 and not a & 0x04


def pcs(state, lo, hi):
    return {state[i] >> 40 for i in range(max(lo, 0), hi)}


class Diff:
    def __init__(self, ours, ref, window, confirm, lookback):
        self.o, self.oc, self.op = ours
        self.r, self.rc, rop = ref
        self.ox = contexts(self.o, self.op)
        self.rx = contexts(self.r, rop)
        self.window, self.confirm, self.lookback = window, confirm, lookback
        self.events = []        # (kind, i, j, di, dj)
        self.cycles = []        # (i, j, ours, ref)
        self.cycle_count = 0
        self.errata = {}        # opcode -> times Atomulator's known error showed
        self.lost = None
        self.tail = None        # (i, j, cycles left) of a forgiven end

    def agree(self, i, j, n):
        """Both agree for n lines, or until the next input read changes
        what they see. Anything shorter is taken for a coincidence: the
        registers of a routine called from two places match for as long
        as the routine runs."""
        o, r = self.o, self.r
        n = min(n, len(o) - i, len(r) - j)
        for k in range(n):
            if o[i + k] != r[j + k] or self.ox[i + k] != self.rx[j + k]:
                return k > 0 and io_read(self.op[i + k - 1], o[i + k - 1]) is not None
        return True

    def resync(self, i, j):
        """The nearest (di, dj) where both agree again. Most resyncs are a
        loop of a few thousand lines, so small windows are tried first."""
        w = 1024
        while True:
            w = min(w, self.window)
            best = self.search(i, j, w)
            if best is not None or w == self.window:
                return best
            w *= 8

    def search(self, i, j, window):
        o, r = self.o, self.r
        where = {}
        for dj in range(min(window, len(r) - j)):
            where.setdefault((r[j + dj], self.rx[j + dj]), []).append(dj)
        best = None
        for di in range(min(window, len(o) - i)):
            if best is not None and di >= best[0] + best[1]:
                break
            for dj in where.get((o[i + di], self.ox[i + di]), ()):
                if best is not None and di + dj >= best[0] + best[1]:
                    break
                if self.agree(i + di, j + dj, self.confirm):
                    best = (di, dj)
                    break
        return best

    def classify(self, i, j, di, dj):
        """The kind, and for `input` the address read."""
        o, r = self.o, self.r
        if i == 0 and j == 0 and all((o[k] ^ r[k]) & ~0xFF00 == 0 for k in range(di)):
            return "reset", None
        if i > 0:
            io = io_read(self.op[i - 1], o[i - 1])
            if io is not None:
                return "input", io
        if interrupted(o, i, di) or interrupted(r, j, dj):
            return "interrupt", None
        if di == dj and all((o[i + k] >> 40) == (r[j + k] >> 40) for k in range(di)):
            return "values", None
        if self.loop_like(o, i, di) and self.loop_like(r, j, dj):
            return "loop", None
        return "path", None

    def loop_like(self, state, lo, n):
        """Every PC in the stretch ran in the lines before it or runs
        more than once in it: a loop that went round more times, or one
        the other side never entered."""
        seen = pcs(state, lo - self.lookback, lo)
        once = set()
        again = set()
        for k in range(lo, lo + n):
            pc = state[k] >> 40
            (again if pc in once else once).add(pc)
        return once - again <= seen

    def run(self):
        o, r, oc, rc, ox, rx = self.o, self.r, self.oc, self.rc, self.ox, self.rx
        i = j = 0
        while i < len(o) and j < len(r):
            if o[i] == r[j] and ox[i] == rx[j]:
                if i + 1 < len(o) and j + 1 < len(r):
                    a, b = oc[i + 1] - oc[i], rc[j + 1] - rc[j]
                    # An interrupt entered after this instruction lands in
                    # the same line on both sides only if both took it
                    # there; the next line's PC says whether they did.
                    if a != b and o[i + 1] == r[j + 1]:
                        op = self.op[i] >> 16
                        if ATOMULATOR_ERRATA.get(op) == (a, b):
                            self.errata[op] = self.errata.get(op, 0) + 1
                        else:
                            self.cycle_count += 1
                            if len(self.cycles) < 20:
                                self.cycles.append((i, j, a, b, op))
                i += 1
                j += 1
                continue
            got = self.resync(i, j)
            if got is None:
                # Both ran the same number of fields, and Atomulator's are
                # longer, so the side that stops first may stop inside a
                # loop the other is still going round. That is the only
                # failed resync forgiven: within TAIL_CYCLES of an end.
                left = min(oc[-1] - oc[i], rc[-1] - rc[j])
                if left < TAIL_CYCLES:
                    self.tail = (i, j, left)
                else:
                    self.lost = (i, j)
                return
            di, dj = got
            kind, io = self.classify(i, j, di, dj)
            self.events.append((kind, i, j, di, dj, io))
            i += di
            j += dj

    def context(self, i, j, di, dj, before=4, show=12):
        o, r = self.o, self.r
        out = []
        for k in range(before, 0, -1):
            if i - k >= 0 and j - k >= 0:
                out.append("    %9d  %s  |  %s" % (i - k, fmt(o[i - k]), fmt(r[j - k])))
        for k in range(min(max(di, dj), show)):
            a = fmt(o[i + k]) if k < di else " " * len(fmt(0))
            b = fmt(r[j + k]) if k < dj else ""
            out.append("  > %9d  %s  |  %s" % (i + k, a, b))
        if max(di, dj) > show:
            out.append("    ... %d ours, %d ref" % (di, dj))
        return "\n".join(out)


EXPECTED = ("reset", "input", "interrupt", "loop")


def report(d, show):
    kinds = dict.fromkeys(EXPECTED + ("values", "path"), 0)
    inputs = {}
    for e in d.events:
        kinds[e[0]] += 1
        if e[5] is not None:
            inputs[e[5]] = inputs.get(e[5], 0) + 1
    print("ours %d lines, ref %d lines" % (len(d.o), len(d.r)))
    print("divergences: " + ", ".join("%d %s" % (n, k) for k, n in kinds.items()))
    if inputs:
        print("  input, by address read: " +
              ", ".join("#%04X %d" % (a, n) for a, n in sorted(inputs.items())))
    print("instructions with different cycles: %d" % d.cycle_count)
    for op, n in sorted(d.errata.items()):
        print("  Atomulator's known timing error: opcode %02X, %d times (%d cycles, not %d)"
              % (op, n, ATOMULATOR_ERRATA[op][1], ATOMULATOR_ERRATA[op][0]))
    print("columns: line  ours  |  ref")
    shown = 0
    for kind, i, j, di, dj, io in d.events:
        if kind in EXPECTED and not show:
            continue
        print("\n%s at ours %d, ref %d: %d ours, %d ref until they agree" % (kind, i, j, di, dj))
        print(d.context(i, j, di, dj))
        shown += 1
        if shown >= 10:
            print("\n(first 10 shown)")
            break
    for i, j, a, b, op in d.cycles[:10]:
        print("\ncycles at ours %d, ref %d: %s opcode %02X takes %d here, %d there"
              % (i, j, fmt(d.o[i]), op, a, b))
    if d.tail:
        i, j, left = d.tail
        print("\nno resync after ours %d, ref %d, %d cycles from the end: forgiven" % (i, j, left))
    if d.lost:
        i, j = d.lost
        print("\nlost at ours %d, ref %d: no resync within %d lines" % (i, j, d.window))
        print(d.context(i, j, min(d.window, 24), min(d.window, 24), show=24))
    return 1 if d.lost or kinds["path"] or kinds["values"] or d.cycle_count else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    runp = sub.add_parser("run", help="trace both machines, then diff")
    runp.add_argument("--keys", default="", help="text typed after boot; \\n is RETURN")
    runp.add_argument("--fields", type=int, help="fields to run (default: boot, keys, 60 more)")
    runp.add_argument("--out", default=str(ROOT / "out" / "trace"))
    runp.add_argument("--ours", default=str(ROOT / "build" / "host" / "test" / "host" / "atom-trace"))
    runp.add_argument("--ref", default=str(ROOT / "out" / "trace" / "atomulator-trace"))
    diffp = sub.add_parser("diff", help="diff two traces already written")
    diffp.add_argument("ours")
    diffp.add_argument("ref")
    for p in (runp, diffp):
        p.add_argument("--window", type=int, default=200000, help="lines searched for a resync")
        p.add_argument("--confirm", type=int, default=256, help="lines that must agree to resync")
        p.add_argument("--lookback", type=int, default=2048, help="lines a loop may reach back")
        p.add_argument("--show-all", action="store_true",
                       help="print the expected divergences too: " + ", ".join(EXPECTED))
    a = ap.parse_args()

    if a.cmd == "run":
        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        text = a.keys.encode().decode("unicode_escape")
        script, end = keyscript(text)
        (out / "keys.txt").write_text(script)
        fields = a.fields or end + 60
        roms = find_roms()
        for exe, name in ((a.ours, "ours"), (a.ref, "ref")):
            if not Path(exe).exists():
                sys.exit("trace-diff: %s is not built (see --help)" % exe)
            extra = ["-s"] if name == "ours" else []
            with open(out / (name + ".trace"), "w") as f:
                got = subprocess.run([exe, *roms, "-n", "100000000", "-f", str(fields),
                                      "-k", str(out / "keys.txt"), *extra],
                                     stdout=f, stderr=subprocess.PIPE, text=True, check=True)
            if extra:
                screen = got.stderr.rstrip("\n")
        print("traced %d fields, keys %r; our screen at the end:" % (fields, text))
        print("\n".join("  | " + line for line in screen.split("\n")))
        ours, ref = out / "ours.trace", out / "ref.trace"
    else:
        ours, ref = a.ours, a.ref

    d = Diff(load(ours), load(ref), a.window, a.confirm, a.lookback)
    d.run()
    sys.exit(report(d, a.show_all))


if __name__ == "__main__":
    main()
