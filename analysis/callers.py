#!/usr/bin/env python3
"""Find direct callers of an address, and scan .text for byte patterns.

Most of BG3's own functions have no symbol, so the relocation-derived xref graph
in bg3.db cannot name them. But a direct `call rel32` still encodes its target,
so scanning .text for E8/E9 displacements that land on an address recovers the
call sites exactly.

  callers.py to <addr>        every direct call/jmp landing on <addr>
  callers.py plt <symbol>     resolve an imported symbol's PLT stub, then its callers
  callers.py count <hexbytes> count occurrences of a byte pattern in .text
"""
import re
import struct
import sys

BIN = "/project/uploads/bg3"
TEXT_VA, TEXT_OFF, TEXT_SZ = 0x218BE00, 0x218AE00, 0x565C316


def text():
    with open(BIN, "rb") as f:
        f.seek(TEXT_OFF)
        return f.read(TEXT_SZ)


def scan_calls(buf, target):
    """Every E8 (call) / E9 (jmp) whose rel32 resolves to target."""
    hits = []
    for op, kind in ((0xE8, "call"), (0xE9, "jmp")):
        start = 0
        while True:
            i = buf.find(bytes([op]), start)
            if i < 0 or i + 5 > len(buf):
                break
            start = i + 1
            rel, = struct.unpack_from("<i", buf, i + 1)
            if TEXT_VA + i + 5 + rel == target:
                hits.append((TEXT_VA + i, kind))
    return sorted(hits)


def plt_stub(name):
    """Locate the PLT stub for an imported symbol via .rela.plt + .got.plt."""
    import subprocess
    out = subprocess.run(["readelf", "-rW", BIN], capture_output=True, text=True).stdout
    got = None
    for line in out.splitlines():
        if name in line and "JUMP_SLO" in line:
            got = int(line.split()[0], 16)
            break
    if got is None:
        return None, None
    # A PLT stub is `ff 25 <rel32>` -> jmp *[rip+rel32], landing on the GOT slot.
    with open(BIN, "rb") as f:
        f.seek(0x77E7140)          # .plt file offset
        plt = f.read(0x3290)
    for i in range(0, len(plt) - 6):
        if plt[i] == 0xFF and plt[i + 1] == 0x25:
            rel, = struct.unpack_from("<i", plt, i + 2)
            if 0x77E8140 + i + 6 + rel == got:
                return 0x77E8140 + i, got
    return None, got


def main():
    cmd = sys.argv[1]
    buf = text()
    if cmd == "to":
        t = int(sys.argv[2], 0)
        hits = scan_calls(buf, t)
        print(f"{len(hits)} direct call/jmp sites -> {t:#x}")
        for a, k in hits:
            print(f"  {a:#010x}  {k}")
    elif cmd == "plt":
        stub, got = plt_stub(sys.argv[2])
        if not stub:
            print(f"no PLT stub for {sys.argv[2]} (got={got})")
            return
        print(f"{sys.argv[2]}: PLT stub {stub:#x}, GOT slot {got:#x}")
        hits = scan_calls(buf, stub)
        print(f"{len(hits)} call sites:")
        for a, k in hits:
            print(f"  {a:#010x}  {k}")
    elif cmd == "count":
        pat = bytes.fromhex(re.sub(r"[^0-9a-fA-F]", "", sys.argv[2]))
        n, start = 0, 0
        first = []
        while True:
            i = buf.find(pat, start)
            if i < 0:
                break
            if len(first) < 5:
                first.append(TEXT_VA + i)
            n += 1
            start = i + 1
        print(f"{n} occurrences of {pat.hex()} in .text")
        for a in first:
            print(f"  {a:#010x}")


main()
