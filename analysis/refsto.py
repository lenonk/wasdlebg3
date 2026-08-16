#!/usr/bin/env python3
"""All code sites referencing an arbitrary DATA address (incl. section-relative
relocations), with the referencing instruction decoded and read/write classified."""
import capstone, sqlite3, struct, sys, collections

con = sqlite3.connect("/project/bg3-extender/analysis/bg3.db")
BIN = "/project/uploads/bg3"


def secs():
    f = open(BIN, "rb"); hdr = f.read(64)
    e_shoff, = struct.unpack_from("<Q", hdr, 0x28)
    esz, enum, _ = struct.unpack_from("<HHH", hdr, 0x3A)
    f.seek(e_shoff); raw = f.read(esz * enum)
    o = []
    for i in range(enum):
        _, _, fl, a, off, sz = struct.unpack_from("<IIQQQQ", raw, i * esz)[:6]
        if a and sz: o.append((a, sz, off))
    return sorted(o)


S = secs()
def read_at(v, n):
    for a, sz, off in S:
        if a <= v < a + sz:
            with open(BIN, "rb") as f:
                f.seek(off + (v - a)); return f.read(n)
    return b""


md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
tgt = int(sys.argv[1], 0)
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 60

secsyms = {i: a for i, a, n in con.execute("SELECT id,addr,name FROM symbols WHERE type='SECTION'")}
q = ",".join(str(i) for i in secsyms)
sites = set()
for site, dst, rt, add in con.execute(
        f"SELECT site,dst,rtype,addend FROM xrefs WHERE dst IN ({q}) AND addend BETWEEN ? AND ?",
        (tgt - 0x8000000, tgt)):
    b = secsyms[dst]
    if b + add + 4 == tgt or b + add == tgt:
        sites.add(site)
for site, in con.execute(
        "SELECT site FROM xrefs WHERE dst IN (SELECT id FROM symbols WHERE addr=? AND type<>'SECTION')",
        (tgt,)):
    sites.add(site)

print(f"[{len(sites)} sites reference {tgt:#x}]")
pat = collections.Counter()
shown = 0
for site in sorted(sites):
    best = None
    for back in range(40, 0, -1):
        d = read_at(site - back, back + 24)
        for ins in md.disasm(d, site - back):
            if ins.address <= site < ins.address + ins.size:
                best = ins; break
            if ins.address > site: break
        if best and best.address + best.size >= site + 4: break
        best = None
    if best is None:
        continue
    ops = best.op_str
    kind = "WRITE" if ops.split(",")[0].strip().startswith(
        ("qword ptr [rip", "dword ptr [rip", "word ptr [rip", "byte ptr [rip")) else "read"
    # next instruction for context
    nxt = ""
    d2 = read_at(best.address + best.size, 24)
    for i2 in md.disasm(d2, best.address + best.size):
        nxt = f"{i2.mnemonic} {i2.op_str}"; break
    pat[(kind, best.mnemonic, nxt)] += 1
    if kind == "WRITE" or shown < limit:
        print(f"{best.address:#010x} {kind} {best.mnemonic} {ops}   ||  {nxt}")
        shown += 1
print("--- pattern histogram ---")
for k, n in pat.most_common(12):
    print(f"{n:>6}  {k}")
