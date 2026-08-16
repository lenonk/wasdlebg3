#!/usr/bin/env python3
"""Recover sizeof(Component) for ECS component types.

Pattern:  movzx ecx, word [rip+TypeId<T,ecs::ComponentTypeIdContext>::m_TypeIndex]
          mov   r8d, <SIZE>
          and   ecx, 0x7fff
          call  0x24760b0     ; GetOrAddComponentStorage(world, handle, storage, typeIdx, size)
Scan a window around each type-index read for `mov r8d, imm` and the call.
"""
import capstone, sqlite3, struct, collections, sys, bisect

con = sqlite3.connect("/project/bg3-extender/analysis/bg3.db")
BIN = "/project/uploads/bg3"
TEXT_BASE = 0x218BE00
TARGET = 0x24760B0


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
fh = open(BIN, "rb")
def read_at(v, n):
    for a, sz, off in S:
        if a <= v < a + sz:
            fh.seek(off + (v - a)); return fh.read(n)
    return b""


md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
ids = {r[0]: r[1] for r in con.execute(
    "SELECT id,dem FROM symbols WHERE name LIKE '_ZN2ls6TypeId%m_TypeIndexE' "
    "AND (dem LIKE '%, ecs::ComponentTypeIdContext>::m_TypeIndex' "
    "  OR dem LIKE '%, ecs::OneFrameComponentTypeIdContext>::m_TypeIndex')")}
q = ",".join("?" * len(ids))
sites = con.execute(f"SELECT site,dst FROM xrefs WHERE dst IN ({q})", list(ids)).fetchall()

tid = [r[0] for r in con.execute("SELECT id FROM symbols WHERE type='SECTION' AND addr=?", (TEXT_BASE,))]
tq = ",".join("?" * len(tid))
calls = sorted(r[0] for r in con.execute(
    f"SELECT site FROM xrefs WHERE dst IN ({tq}) AND addend=?", tid + [TARGET - TEXT_BASE - 4]))

res = collections.defaultdict(collections.Counter)
for site, dst in sites:
    i = bisect.bisect_left(calls, site)
    if i >= len(calls) or calls[i] - site > 48:
        continue
    end = calls[i] + 4
    data = read_at(site - 8, end - site + 12)
    size = None
    for ins in md.disasm(data, site - 8):
        if ins.address > end: break
        if ins.mnemonic == "mov" and ins.op_str.startswith("r8d, 0x"):
            try: size = int(ins.op_str.split(",")[1].strip(), 16)
            except ValueError: pass
        elif ins.mnemonic == "mov" and ins.op_str.startswith("r8d,") and ins.op_str[4:].strip().isdigit():
            size = int(ins.op_str.split(",")[1])
    if size is not None:
        name = ids[dst].split("ls::TypeId<")[1].rsplit(", ecs::", 1)[0]
        res[name][size] += 1

print(f"# {len(res)} component types with a recovered size")
for name in sorted(res):
    c = res[name]
    best, n = c.most_common(1)[0]
    flag = "" if len(c) == 1 else f"   (!! also {dict(c)})"
    print(f"{best:>6}  {name}{flag}")
