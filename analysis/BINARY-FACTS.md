# What we know about the native-Linux BG3 binary

Everything here was observed directly in `/project/uploads/bg3`. Reproduce any line with the
commands in `bg3q.py` or the raw `readelf` invocations noted below.

## Identity

Native x86-64 ELF, PIE, dynamically linked, **not stripped** in the ELF sense — 152,416 symbols
survive in `.symtab`. 214 MB. Build ID `c8da62b137caebd03b57c23b65ba2547745ce851`.

It is genuinely Baldur's Gate 3, Patch-8 era: `.rodata` carries `Baldur's Gate 3`, `GustavX`
(the Patch 8 mod folder), `bg3nat1–8.larian.com`, and the Larian modding-terms URLs.

Toolchain, from `.comment`: clang 18.1.8 + LLD 18.1.8, with PhysX built by clang 11, ISPC 1.19,
and some objects from GCC 12.2. Namespaces follow Larian's Divinity engine convention:

| prefix  | meaning                       |
|---------|-------------------------------|
| `ecl::` | client                        |
| `esv::` | server                        |
| `eoc::` | shared gameplay ("EoCApp")    |
| `ls::`  | Larian core library           |
| `ecs::` | the entity-component-system   |

Runtime dependencies (`readelf -d`): `libSDL2.so`, `libsteam_api.so`, `libBink2x64.so`,
**`libOsiris.so`**, `libnvsdk_ngx.so` (DLSS), `libssl/libcrypto 1.1`, with `RUNPATH=$ORIGIN`.
We have **only the executable** — none of these libraries, and no game data.

## What the symbols actually give us (and what they don't)

This is the crucial nuance, and it is the opposite of what "not stripped" suggests.

**Larian's own functions have no names.** Of 45,184 sized `FUNC` symbols, essentially all belong to
statically linked third-party code — Noesis GUI (19,286), PhysX (5,512), PlayFab, Wwise/`AK::`,
asio, websocketpp, crashpad, Granny. They cover only **9.1 % of `.text`** (8.3 MB of 90.5 MB).
The other ~90 % is anonymous Larian engine code. There are exactly 77 `ls::` `FUNC` symbols and
zero for `ecl::`/`esv::`/`eoc::`.

There is also **no RTTI to fall back on**: `_ZTV*`/`_ZTI*` counts for `ecl::`, `esv::`, `eoc::`
and `ls::` are all zero.

What we get instead is better suited to a mod anyway:

**1. An exact cross-reference graph.** The binary was linked with `--emit-relocs`, so `.rela.text`
survived — 71.7 MB holding **2,986,688 relocations**, one per code site that references a symbol.
Every call target, data reference and string reference is recorded *by symbol*, with none of the
guesswork static disassembly normally involves. All 2,986,688 sites land inside `.text`, verified.

**2. Named ECS types with live runtime indices.** clang emitted a static for every ECS type
instantiation:

```
_ZN2ls6TypeIdIN3ecl9CharacterEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE
  = ls::TypeId<ecl::Character, ecs::ComponentTypeIdContext>::m_TypeIndex
```

That is a 4-byte static, at an address we know, holding the type index the ECS assigns **at
runtime**. This yields **2,107 named component types and 934 named systems** — including
`eoc::MovementComponent`, `eoc::SteeringComponent`, `eoc::PathingComponent`, `eoc::CanMoveComponent`,
`ecl::MovementSystem`, `ecl::steering::SSSteerSystem`, `ecl::movement::DashingSystem`.

This is a significant advantage over the Windows build, where the Script Extender must pattern-scan
for these and re-scan after every patch.

**3. String-to-code attribution.** 26,004 `.L.str` literals are themselves symbols, so string
*content* maps to the exact instruction that uses it — which is how anonymous Larian functions get
identified. 79,669 `.L*` local symbols in total.

## Input surface

SDL2 is the input layer, and 71 `SDL_*` symbols are imported through the PLT — including
`SDL_PollEvent` and the whole `SDL_GameController*` family (`Open`, `GetType`, `Rumble`, `SetLED`,
`GetJoystick`, `HasLED`…). Because these are ordinary dynamic imports, `LD_PRELOAD` interposition
reaches them with no inline hooking at all.

## Tooling built here

- `build_index.py` — parses `.symtab` and `.rela.text` into `bg3.db` (SQLite: `symbols`, `xrefs`).
  Runs in about a minute.
- `bg3q.py` — query tool: `sym`, `comp`, `at`, `to`, `from`, `str`, `dis`. Disassembly is
  annotated with reloc-derived symbol names, and `str` resolves literal content to the code using it.
- Ghidra 12.1.2 at `/opt/re/ghidra_12.1.2_PUBLIC/` with JDK 21, for decompiling narrow ranges.
  Full auto-analysis of a 214 MB binary would take hours and is not the primary path.

## Legal / distribution note

`bg3.db` and `symtab.raw.txt` are derived from Larian's copyrighted executable. They are local
working notes and are gitignored. Nothing derived from the binary gets published; the mod ships as
source that reads the user's own installed copy.
