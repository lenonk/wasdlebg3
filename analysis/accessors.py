#!/usr/bin/env python3
"""For every code site that reads a ls::TypeId<T, ecs::*>::m_TypeIndex static,
find the next .text-relative call within +64 bytes and tally the targets.
That enumerates the ECS accessor API surface."""
import sqlite3, sys, collections, re

con = sqlite3.connect("/project/bg3-extender/analysis/bg3.db")
TEXT_BASE = 0x218BE00
TEXT_IDS = [r[0] for r in con.execute(
    "SELECT id FROM symbols WHERE type='SECTION' AND addr=?", (TEXT_BASE,))]

ctx = sys.argv[1] if len(sys.argv) > 1 else "ComponentTypeIdContext"
like = f"%, ecs::{ctx}>::m_TypeIndex"
ids = {r[0]: r[1] for r in con.execute(
    "SELECT id,dem FROM symbols WHERE name LIKE '_ZN2ls6TypeId%m_TypeIndexE' AND dem LIKE ?", (like,))}
print(f"{len(ids)} type-index statics for {ctx}", file=sys.stderr)

q = ",".join("?" * len(ids))
sites = con.execute(f"SELECT site,dst FROM xrefs WHERE dst IN ({q})", list(ids)).fetchall()
print(f"{len(sites)} read sites", file=sys.stderr)

# build a sorted list of all text-target relocs for fast window search
tq = ",".join("?" * len(TEXT_IDS))
calls = con.execute(
    f"SELECT site,addend,rtype FROM xrefs WHERE dst IN ({tq}) AND rtype IN (2,4) ORDER BY site",
    TEXT_IDS).fetchall()
import bisect
csites = [c[0] for c in calls]
print(f"{len(calls)} intra-text call relocs", file=sys.stderr)

tally = collections.Counter()
examples = {}
for site, dst in sites:
    i = bisect.bisect_left(csites, site)
    while i < len(calls) and calls[i][0] - site <= 64:
        tgt = TEXT_BASE + calls[i][1] + 4
        tally[tgt] += 1
        examples.setdefault(tgt, []).append((site, ids[dst]))
        break
    else:
        continue

for tgt, n in tally.most_common(30):
    ex = examples[tgt][0]
    print(f"{tgt:#010x}  {n:>6}   e.g. site {ex[0]:#x}  {ex[1][11:80]}")
