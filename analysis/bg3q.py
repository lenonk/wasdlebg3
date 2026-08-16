#!/usr/bin/env python3
"""Query tool for the BG3 Linux binary analysis database.

Subcommands:
  sym <regex>          search symbols by demangled name
  comp <regex>         search ECS component / system type names
  at <addr>            what lives at an address (containing/nearest symbol)
  to <name|addr>       code sites that reference this symbol  (xrefs TO)
  from <name|addr>     symbols a function references           (xrefs FROM)
  str <regex>          search string literals; shows code sites using them
  dis <addr> [count]   disassemble at a virtual address
  fn <addr>            disassemble the whole containing function (best effort)
"""
import bisect
import re
import sqlite3
import struct
import sys

DB = "/project/bg3-extender/analysis/bg3.db"
BIN = "/project/uploads/bg3"
_con = None


def con():
    global _con
    if _con is None:
        _con = sqlite3.connect(DB)
    return _con


def sections():
    f = open(BIN, "rb")
    hdr = f.read(64)
    e_shoff, = struct.unpack_from("<Q", hdr, 0x28)
    e_shentsize, e_shnum, _ = struct.unpack_from("<HHH", hdr, 0x3A)
    f.seek(e_shoff)
    raw = f.read(e_shentsize * e_shnum)
    out = []
    for i in range(e_shnum):
        _, _, flags, addr, off, size = struct.unpack_from("<IIQQQQ", raw, i * e_shentsize)[:6]
        if addr and size:
            out.append((addr, size, off))
    return sorted(out)


_SECS = None


def v2o(vaddr):
    """Virtual address -> file offset."""
    global _SECS
    if _SECS is None:
        _SECS = sections()
    for a, sz, off in _SECS:
        if a <= vaddr < a + sz:
            return off + (vaddr - a)
    return None


def read_at(vaddr, n):
    o = v2o(vaddr)
    if o is None:
        return None
    with open(BIN, "rb") as f:
        f.seek(o)
        return f.read(n)


def parse_addr(s):
    try:
        return int(s, 0)
    except ValueError:
        r = con().execute(
            "SELECT addr FROM symbols WHERE name=? OR dem=? ORDER BY addr DESC LIMIT 1", (s, s)
        ).fetchone()
        return r[0] if r else None


def sym_at(addr):
    """Containing sized symbol, else nearest preceding."""
    r = con().execute(
        "SELECT dem,addr,size,type FROM symbols WHERE addr<=? AND addr>0 ORDER BY addr DESC LIMIT 12",
        (addr,)).fetchall()
    for dem, a, sz, t in r:
        if sz and a <= addr < a + sz:
            return f"{dem} +{addr - a:#x} ({t})"
    return f"~{r[0][0]} +{addr - r[0][1]:#x}" if r else "?"


def c_sym(pat):
    rx = re.compile(pat, re.I)
    n = 0
    for dem, a, sz, t in con().execute(
            "SELECT dem,addr,size,type FROM symbols WHERE addr>0 ORDER BY addr"):
        if rx.search(dem):
            print(f"{a:#010x} {sz:>7} {t:<7} {dem}")
            n += 1
            if n >= 400:
                print("... (truncated at 400)")
                break
    print(f"[{n} hits]", file=sys.stderr)


def c_comp(pat):
    rx = re.compile(pat, re.I)
    q = re.compile(r"^ls::TypeId<(.+), ecs::(\w+)>::m_TypeIndex")
    for dem, a in con().execute(
            "SELECT dem,addr FROM symbols WHERE name LIKE '_ZN2ls6TypeId%m_TypeIndexE' ORDER BY dem"):
        m = q.match(dem)
        if m and rx.search(m.group(1)):
            print(f"{a:#010x}  [{m.group(2)}]  {m.group(1)}")


def c_to(target):
    a = parse_addr(target)
    ids = [r[0] for r in con().execute(
        "SELECT id FROM symbols WHERE addr=? OR name=? OR dem=?", (a, target, target))]
    if not ids:
        print("no such symbol")
        return
    q = ",".join("?" * len(ids))
    rows = con().execute(
        f"SELECT site,src FROM xrefs WHERE dst IN ({q}) ORDER BY site", ids).fetchall()
    print(f"[{len(rows)} references]")
    for site, src in rows[:300]:
        s = con().execute("SELECT dem FROM symbols WHERE id=?", (src,)).fetchone() if src >= 0 else None
        print(f"  {site:#010x}  in {s[0][:90] if s else sym_at(site)}")


def c_from(target):
    a = parse_addr(target)
    r = con().execute(
        "SELECT id,dem,addr,size FROM symbols WHERE type='FUNC' AND size>0 AND addr<=? "
        "ORDER BY addr DESC LIMIT 1", (a,)).fetchone()
    lo, hi = (r[2], r[2] + r[3]) if r and r[2] <= a < r[2] + r[3] else (a, a + 0x400)
    print(f"[range {lo:#x}-{hi:#x}]")
    for site, dst, add in con().execute(
            "SELECT site,dst,addend FROM xrefs WHERE site BETWEEN ? AND ? ORDER BY site", (lo, hi)):
        d = con().execute("SELECT dem,name FROM symbols WHERE id=?", (dst,)).fetchone()
        nm = d[0] if d else "?"
        if nm.startswith(".L.str"):
            b = read_at(con().execute("SELECT addr FROM symbols WHERE id=?", (dst,)).fetchone()[0], 80)
            nm += "  =  " + repr(b.split(b"\0")[0].decode("utf-8", "replace"))
        print(f"  {site:#010x} -> {nm[:110]}")


def c_str(pat):
    rx = re.compile(pat, re.I)
    rows = con().execute(
        "SELECT id,addr,size FROM symbols WHERE name LIKE '.L.str%' AND size>0").fetchall()
    hits = 0
    for i, a, sz in rows:
        b = read_at(a, min(sz, 300))
        if not b:
            continue
        s = b.split(b"\0")[0].decode("utf-8", "replace")
        if rx.search(s):
            xr = con().execute("SELECT site,src FROM xrefs WHERE dst=?", (i,)).fetchall()
            print(f'{a:#010x} "{s[:120]}"')
            for site, src in xr[:8]:
                print(f"      used at {site:#010x}  in {sym_at(site)[:90]}")
            hits += 1
            if hits >= 60:
                print("... truncated")
                break
    print(f"[{hits} strings]", file=sys.stderr)


def c_dis(addr, count=60):
    import capstone
    count = int(count)
    a = parse_addr(addr)
    data = read_at(a, count * 8 + 64)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False
    xr = {s: d for s, d in con().execute(
        "SELECT site,dst FROM xrefs WHERE site BETWEEN ? AND ?", (a, a + count * 8 + 64))}
    print(f"; {sym_at(a)}")
    for i, ins in enumerate(md.disasm(data, a)):
        if i >= count:
            break
        note = ""
        for off in range(ins.size):
            if ins.address + off in xr:
                d = con().execute("SELECT dem FROM symbols WHERE id=?",
                                  (xr[ins.address + off],)).fetchone()
                if d:
                    nm = d[0]
                    if nm.startswith(".L.str"):
                        sa = con().execute("SELECT addr FROM symbols WHERE id=?",
                                           (xr[ins.address + off],)).fetchone()[0]
                        bb = read_at(sa, 80)
                        nm = "str " + repr(bb.split(b"\0")[0].decode("utf-8", "replace")[:60])
                    note = "   ; " + nm[:100]
        print(f"{ins.address:#010x}  {ins.mnemonic:<9} {ins.op_str:<48}{note}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd, args = sys.argv[1], sys.argv[2:]
    {"sym": c_sym, "comp": c_comp, "to": c_to, "from": c_from,
     "str": c_str, "dis": c_dis, "at": lambda a: print(sym_at(parse_addr(a)))}[cmd](*args) \
        if cmd in {"sym", "comp", "to", "from", "str", "dis", "at"} else print(__doc__)


main()
