#!/usr/bin/env python3
"""For each xref to a symbol, decode the referencing instruction and say
whether it READS or WRITES the target (mov [rip+x], r  => write)."""
import capstone, sqlite3, struct, sys

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
target = sys.argv[1]
a = int(target, 0)
ids = [r[0] for r in con.execute("SELECT id FROM symbols WHERE addr=?", (a,))]
q = ",".join("?" * len(ids))
for (site,) in con.execute(f"SELECT site FROM xrefs WHERE dst IN ({q}) ORDER BY site", ids):
    # linear-sweep from several start offsets; pick decode covering `site`
    best = None
    for back in range(40, 0, -1):
        d = read_at(site - back, back + 24)
        for ins in md.disasm(d, site - back):
            if ins.address <= site < ins.address + ins.size:
                best = ins
                break
            if ins.address > site:
                break
        if best and best.address + best.size >= site + 4:
            break
        best = None
    if best is None:
        print(f"{site:#010x} ????")
        continue
    w = "WRITE" if best.op_str.split(",")[0].strip().endswith("[rip + 0x%x]" % 0) or \
        best.op_str.startswith(("qword ptr [rip", "dword ptr [rip", "word ptr [rip", "byte ptr [rip")) else "read "
    print(f"{best.address:#010x} {w} {best.mnemonic} {best.op_str}")
