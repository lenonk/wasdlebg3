#!/usr/bin/env python3
"""Build a SQLite index of BG3's symbols and cross-references.

The shipped binary was linked with --emit-relocs, so .rela.text survives: one
relocation per code-site that references a symbol. That gives an exact xref
graph (call edges, vtable refs, string refs) without disassembling anything.
"""
import bisect
import os
import sqlite3
import struct
import subprocess
import sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "/project/uploads/bg3"
OUT = sys.argv[2] if len(sys.argv) > 2 else "/project/bg3-extender/analysis/bg3.db"

STT = {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION", 4: "FILE", 6: "TLS", 10: "GNU_IFUNC"}
STB = {0: "LOCAL", 1: "GLOBAL", 2: "WEAK"}


def read_sections(f):
    f.seek(0)
    hdr = f.read(64)
    e_shoff, = struct.unpack_from("<Q", hdr, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", hdr, 0x3A)
    f.seek(e_shoff)
    raw = f.read(e_shentsize * e_shnum)
    secs = []
    for i in range(e_shnum):
        (name, stype, flags, addr, off, size, link, info, align,
         entsize) = struct.unpack_from("<IIQQQQIIQQ", raw, i * e_shentsize)
        secs.append(dict(nameoff=name, type=stype, flags=flags, addr=addr,
                         off=off, size=size, link=link, info=info, entsize=entsize))
    shstr = secs[e_shstrndx]
    f.seek(shstr["off"])
    strtab = f.read(shstr["size"])
    for s in secs:
        end = strtab.index(b"\0", s["nameoff"])
        s["name"] = strtab[s["nameoff"]:end].decode()
    return secs


def cstr(buf, off):
    end = buf.index(b"\0", off)
    return buf[off:end].decode("utf-8", "replace")


def demangle_bulk(names):
    """Run every mangled name through c++filt in one pass."""
    mangled = [n for n in names if n.startswith("_Z")]
    out = {}
    if not mangled:
        return out
    CH = 20000
    for i in range(0, len(mangled), CH):
        chunk = mangled[i:i + CH]
        p = subprocess.run(["c++filt", "-n"], input="\n".join(chunk),
                           capture_output=True, text=True)
        lines = p.stdout.split("\n")
        for m, d in zip(chunk, lines):
            if d and d != m:
                out[m] = d
        print(f"  demangled {min(i+CH, len(mangled))}/{len(mangled)}", file=sys.stderr)
    return out


def main():
    if os.path.exists(OUT):
        os.remove(OUT)
    f = open(BIN, "rb")
    secs = read_sections(f)
    by_name = {s["name"]: s for s in secs}
    text = by_name[".text"]

    symtab = by_name[".symtab"]
    strsec = secs[symtab["link"]]
    f.seek(strsec["off"])
    strbuf = f.read(strsec["size"])
    f.seek(symtab["off"])
    symbuf = f.read(symtab["size"])
    nsym = symtab["size"] // 24
    print(f"[*] {nsym} symbols", file=sys.stderr)

    syms = []
    for st_name, st_info, st_other, st_shndx, st_value, st_size in \
            struct.iter_unpack("<IBBHQQ", symbuf):
        syms.append((cstr(strbuf, st_name), st_info & 0xF, st_info >> 4,
                     st_value, st_size, st_shndx))

    dem = demangle_bulk([s[0] for s in syms])

    con = sqlite3.connect(OUT)
    con.executescript("""
      PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;
      CREATE TABLE symbols(id INTEGER PRIMARY KEY, name TEXT, dem TEXT,
                           type TEXT, bind TEXT, addr INTEGER, size INTEGER, shndx INTEGER);
      CREATE TABLE xrefs(site INTEGER, src INTEGER, dst INTEGER, rtype INTEGER, addend INTEGER);
    """)
    con.executemany(
        "INSERT INTO symbols(id,name,dem,type,bind,addr,size,shndx) VALUES(?,?,?,?,?,?,?,?)",
        [(i, n, dem.get(n, n), STT.get(t, str(t)), STB.get(b, str(b)), v, sz, sh)
         for i, (n, t, b, v, sz, sh) in enumerate(syms)])

    # Address -> containing FUNC, for attributing each relocation site to a caller.
    funcs = sorted((v, sz, i) for i, (n, t, b, v, sz, sh) in enumerate(syms)
                   if t == 2 and v and sz)
    faddr = [x[0] for x in funcs]
    print(f"[*] {len(funcs)} sized functions", file=sys.stderr)

    rela = by_name.get(".rela.text")
    edges = []
    if rela:
        f.seek(rela["off"])
        nrel = rela["size"] // 24
        print(f"[*] {nrel} relocations in .rela.text", file=sys.stderr)
        CH = 24 * 200000
        done = 0
        remaining = rela["size"]
        while remaining:
            buf = f.read(min(CH, remaining))
            if not buf:
                break
            remaining -= len(buf)
            for r_off, r_info, r_add in struct.iter_unpack("<QQq", buf):
                dst = r_info >> 32
                if not dst:
                    continue
                j = bisect.bisect_right(faddr, r_off) - 1
                src = -1
                if j >= 0:
                    a, sz, idx = funcs[j]
                    if r_off < a + sz:
                        src = idx
                edges.append((r_off, src, dst, r_info & 0xFFFFFFFF, r_add))
            done += len(buf) // 24
            if len(edges) > 400000:
                con.executemany("INSERT INTO xrefs VALUES(?,?,?,?,?)", edges)
                edges = []
            print(f"  reloc {done}/{nrel}", file=sys.stderr)
    if edges:
        con.executemany("INSERT INTO xrefs VALUES(?,?,?,?,?)", edges)

    print("[*] indexing", file=sys.stderr)
    con.executescript("""
      CREATE INDEX i_sym_name ON symbols(name);
      CREATE INDEX i_sym_addr ON symbols(addr);
      CREATE INDEX i_xref_src ON xrefs(src);
      CREATE INDEX i_xref_dst ON xrefs(dst);
    """)
    con.commit()
    n = con.execute("SELECT COUNT(*) FROM xrefs").fetchone()[0]
    print(f"[+] {OUT}: {nsym} symbols, {n} xrefs", file=sys.stderr)
    con.close()


main()
