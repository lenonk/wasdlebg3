#!/usr/bin/env python3
"""Reverse/forward xrefs for ANONYMOUS .text targets.

The binary keeps .rela.text (--emit-relocs). Calls into unnamed Larian code
relocate against the .text SECTION symbol with addend = target - text_base - 4
(R_X86_64_PLT32/PC32). So we can do exact call-graph queries on unnamed funcs.

  anon.py callers <addr>     every code site that calls/references <addr>
  anon.py callees <addr> [n] every call target inside <addr>..<addr>+n (default 0x400)
  anon.py fn <addr>          heuristic function extent + callees + string refs
"""
import re, sqlite3, struct, sys

DB = "/project/bg3-extender/analysis/bg3.db"
BIN = "/project/uploads/bg3"
con = sqlite3.connect(DB)

# .text section symbol(s)
TEXT_BASE = 0x218BE00
TEXT_END = TEXT_BASE + 0x565C316
TEXT_IDS = [r[0] for r in con.execute(
    "SELECT id FROM symbols WHERE type='SECTION' AND addr=?", (TEXT_BASE,))]


def secs():
    f = open(BIN, "rb")
    hdr = f.read(64)
    e_shoff, = struct.unpack_from("<Q", hdr, 0x28)
    esz, enum, _ = struct.unpack_from("<HHH", hdr, 0x3A)
    f.seek(e_shoff); raw = f.read(esz * enum)
    out = []
    for i in range(enum):
        _, _, fl, addr, off, size = struct.unpack_from("<IIQQQQ", raw, i * esz)[:6]
        if addr and size:
            out.append((addr, size, off))
    return sorted(out)


_S = None
def read_at(v, n):
    global _S
    if _S is None: _S = secs()
    for a, sz, off in _S:
        if a <= v < a + sz:
            with open(BIN, "rb") as f:
                f.seek(off + (v - a)); return f.read(n)
    return b""


def sym_at(addr):
    r = con.execute("SELECT dem,addr,size,type FROM symbols WHERE addr<=? AND addr>0 "
                    "ORDER BY addr DESC LIMIT 20", (addr,)).fetchall()
    for dem, a, sz, t in r:
        if sz and a <= addr < a + sz:
            return f"{dem}+{addr-a:#x}"
    return f"~{r[0][0]}+{addr-r[0][1]:#x}" if r else "?"


def name_of(dstid, addend, rtype):
    row = con.execute("SELECT dem,name,type,addr FROM symbols WHERE id=?", (dstid,)).fetchone()
    if not row: return "?"
    dem, name, t, addr = row
    if t == 'SECTION':
        tgt = addr + addend + (4 if rtype in (2, 4) else 0)
        s = sym_at(tgt)
        if TEXT_BASE <= tgt < TEXT_END:
            return f"ANON {tgt:#x}   [{s}]"
        b = read_at(tgt, 120)
        if b and all(32 <= c < 127 or c in (9, 10) for c in b.split(b"\0")[0][:60]) and b[0:1] != b"\0":
            return f"DATA {tgt:#x} = {b.split(b'\\0')[0][:70]!r}"
        return f"DATA {tgt:#x}   [{s}]"
    if name.startswith(".L.str"):
        b = read_at(addr, 160)
        return f"str {b.split(b'\\0')[0].decode('utf8','replace')[:100]!r}"
    return dem


def callers(addr):
    a = int(addr, 0)
    q = ",".join("?" * len(TEXT_IDS))
    rows = []
    for rt, delta in ((4, -4), (2, -4), (1, 0), (11, 0)):
        rows += con.execute(
            f"SELECT site,rtype FROM xrefs WHERE dst IN ({q}) AND addend=? AND rtype=?",
            TEXT_IDS + [a - TEXT_BASE + delta, rt]).fetchall()
    # also direct-named symbol refs
    ids = [r[0] for r in con.execute("SELECT id FROM symbols WHERE addr=?", (a,))]
    if ids:
        qq = ",".join("?" * len(ids))
        rows += [(s, r) for s, r in con.execute(
            f"SELECT site,rtype FROM xrefs WHERE dst IN ({qq})", ids)]
    rows.sort()
    print(f"[{len(rows)} refs to {a:#x}]")
    for site, rt in rows[:200]:
        print(f"  {site:#010x} rt={rt:<3} in {sym_at(site)[:100]}")


def callees(addr, n=0x400):
    a = int(addr, 0); n = int(str(n), 0)
    for site, dst, rt, add in con.execute(
            "SELECT site,dst,rtype,addend FROM xrefs WHERE site BETWEEN ? AND ? ORDER BY site",
            (a, a + n)):
        print(f"  {site:#010x} -> {name_of(dst, add, rt)[:150]}")


if __name__ == "__main__":
    cmd = sys.argv[1]
    {"callers": callers, "callees": callees}[cmd](*sys.argv[2:])
