# wasdlebg3

**WASD movement for the native Linux build of Baldur's Gate 3.** No Proton, no Windows
Script Extender, no remapping your keybinds.

Your character walks with W/A/S/D through the game's *own* movement code — the same path
an analog stick drives — so you get real locomotion, animation and collision rather than
synthesised mouse clicks. Movement is camera-relative: forward means away from the camera.

Want mouse-look too? [**rghtclkcamrotatelebg3**](https://github.com/Nerocon/rghtclkcamrotatelebg3)
adds hold-right-mouse camera rotation. See [Running both](#running-both).

Ships as a single `LD_PRELOAD` library, `bg3le.so`. It patches nothing on disk and leaves
no trace after you quit.

Built on [**bg3lese**](https://github.com/Nerocon/bg3lese), a script-extender host for
native-Linux BG3. This is its first plugin; the extender is statically linked in, so
there is still only one file to install.

> **Status:** working and in use. Version 0.3.1. Single-player only — see
> [Scope and safety](#scope-and-safety).

---

## Requirements

- The **native Linux** build of Baldur's Gate 3. If your install only has `bg3.exe`,
  this is not for you — that's the Windows build, and Norbyte's
  [Script Extender](https://github.com/Norbyte/bg3se) is what you want instead.
- glibc 2.34 or newer (any distro from 2021 onward).
- SDL2 headers if you build from source (`libsdl2-dev` / `SDL2-devel`).

Verify your game is supported before doing anything else:

```sh
./bg3le-check "/path/to/Baldurs Gate 3"
```

Point it at the game folder or the executable — given a folder it finds `bin/bg3` itself.
You want `GOOD — this build is supported`. If it says otherwise, please
[open an issue](https://github.com/Nerocon/wasdlebg3/issues) with the output; that tool
prints everything needed to add support for another build.

## Install

```sh
git clone --recursive https://github.com/Nerocon/wasdlebg3
cd wasdlebg3
make
./install.sh
```

(`--recursive` pulls in the extender. If you forget, `make` runs
`git submodule update --init` for you.)

`install.sh` copies the library to `~/.local/share/bg3le/` and prints the exact Steam
launch options to paste. Or do it by hand:

```sh
mkdir -p ~/bg3le && cp build/bg3le.so ~/bg3le/
```

Then set this as the game's **Steam launch options** (Properties → General):

```
LD_PRELOAD=/home/YOURNAME/bg3le/bg3le.so %command%
```

Use an absolute path with no spaces in it — Steam launch options cannot be quoted, which
is why the library does not live in the game folder (that path has spaces).

Launch the game and hold **W**. That's it.

## Running both

The default build has the extender linked in, so it is one file — but for that reason you
must **not** preload two such mods at once: each contains a host and both would interpose
`SDL_PollEvent` and fight over it.

To run this alongside the camera mod, use the plugin builds instead. One host, many
plugins, cooperating over the input stream:

```sh
make plugin                       # in each mod's checkout
mkdir -p ~/bg3le/plugins
cp build/plugins/wasd.so ~/bg3le/plugins/
cp ../rghtclkcamrotatelebg3/build/plugins/camlook.so ~/bg3le/plugins/
cp vendor/bg3lese/build/bg3lese.so ~/bg3le/
```

```
LD_PRELOAD=/home/YOURNAME/bg3le/bg3lese.so %command%
```

The host loads every `.so` in `plugins/` beside it, refuses any built against a different
ABI, and refuses a second copy of a plugin it already loaded.

## With bg3le

If you run [bg3le](https://github.com/lenonk/bg3le), the native-Linux script extender,
this mod also builds as a bg3le plugin, `linux_native_wasd.so`. bg3le loads it from its
plugins directory, so there is no launch option to set, and its options are bg3le
settings. `make bg3le` builds it and `bg3le/package.sh` makes the release zip; see
[bg3le/README.md](bg3le/README.md). Don't preload this mod's `bg3le.so` as well.

## Controls

| input | what it does |
|---|---|
| **W A S D** | move, relative to the camera |
| **Left Shift** (hold) | walk instead of run |
| everything else | untouched — jump, interact, hotbar and camera all work normally |

WASD are hidden from the game while you move, so they no longer pan the camera. Everything
else passes through in order. Typing in a text field (naming a save, renaming a character)
temporarily disables movement so your keystrokes reach the box, and losing window focus
releases movement so alt-tabbing with W held doesn't walk you into a lake.

## Configuration

All configuration is environment variables, so it fits in a Steam launch option. Defaults
are what you want; the rest are for troubleshooting.

| variable | default | meaning |
|---|---|---|
| `BG3LE_WALK_KEY` | `lshift` | walk modifier: `lshift` `rshift` `lctrl` `rctrl` `lalt` `ralt` `capslock` `tab` `none` |
| `BG3LE_WALK_SPEED` | `0.5` | walk speed as a fraction of run, between 0 and 1 |
| `BG3LE_SUPPRESS` | `1` | hide WASD from the game's own bindings |
| `BG3LE_MOVE` | `1` | drive movement (`0` = observe only) |
| `BG3LE_GATE` | `1` | open the game's controller-mode gate (`0` disables the mod) |
| `BG3LE_INPUT_ONLY` | `0` | filter input without moving, for diagnosing an unrecognised build |
| `BG3LE_LOG` | `/tmp/bg3le.log` | log file |
| `BG3LE_TRACE` | `0` | per-keystroke and per-frame detail |
| `BG3LE_VERBOSE` | `0` | also log from Steam's helper processes |

Example — walk on Ctrl at 30% speed:

```
BG3LE_WALK_KEY=lctrl BG3LE_WALK_SPEED=0.3 LD_PRELOAD=/home/YOURNAME/bg3le/bg3le.so %command%
```

## Troubleshooting

Run `./diagnose.sh`. It is read-only, launches nothing, and reports whether the library
loads, your glibc version, every log on the system, the launch options **as Steam actually
stored them**, whether a running BG3 is native or Proton, and what your install contains.
Paste the whole output into an issue.

A few specifics:

**No log file at all.** The library opens its log before it decides anything, so a missing
file means `LD_PRELOAD` never reached any process — almost always the launch options. Check
`diagnose.sh` section 5, which reads what Steam saved rather than what you think you typed.
Close Steam first; it writes that file lazily.

**The log exists but says nothing.** Steam re-execs through a dozen helper processes and the
library stays silent in all of them. `BG3LE_VERBOSE=1` shows them.

**Movement does nothing.** Set `BG3LE_TRACE=1` and look for `opened 1 of 1 movement gate(s)`
and `movement control live`. If the gate did not open, the game build likely differs from
what the signature expects — run `bg3le-check` and file the output.

**Bypassing Steam entirely.** `./run-bg3le.sh "/path/to/Baldurs Gate 3"` launches the game
directly with the right environment, which removes Steam's launch options and container from
the picture. Leave Steam running in the background.

## Scope and safety

**Single-player.** The client streams a derived position and heading to the server, and
injecting movement in co-op is untested and plausibly desyncs. Don't.

**Saves are not at risk** in any way we can see: the mod writes one transient input vector
per frame and touches no persistent state. It also patches six bytes of code in memory —
never on disk — and restores them when the game exits.

**Not a cheat.** It drives the same input path a gamepad uses. It does not alter stats,
rolls, speed beyond the game's own run speed, or anything the server validates.

## How it works

The extender half — interposing `SDL_PollEvent`, resolving symbols, scanning for patterns
and patching safely — is [bg3lese](https://github.com/Nerocon/bg3lese) and documented
there. What follows is what belongs to *this* mod.

Two facts about the binary carry it, each verified against it:

1. **A forced-input override already exists.** The movement-input fetch checks it *before*
   polling `CharacterMoveForward/Backward/Left/Right`, and it is consumed upstream of the
   deadzone, normalise and camera rotation. So we write a camera-relative vector exactly
   like stick deflection — and because magnitudes below 1.0 survive the normalise step, a
   shorter vector is a genuine walk rather than a clamped run.

2. **That path is gated by one branch.** A `cmp`/`je` on a controller-mode flag skips it
   entirely outside controller mode. Writing the flag loses a per-frame race against the
   engine's input-mode arbiter — measured, it reset ours on every single frame — so the
   six-byte branch is NOPed instead, as a single aligned atomic store.

`DESIGN.md` has the full reverse-engineering write-up, including which claims were verified
directly and which remain assumptions. `analysis/` holds the tooling: a SQLite index of the
binary's 152,416 symbols and 2,986,688 relocation-derived cross-references, plus query and
disassembly helpers.

## Building and testing

```sh
make          # builds build/bg3le.so and build/bg3le-check
make test     # 23 assertions here, plus 54 in the extender
```

The test suite needs **neither a copy of BG3 nor a display**. Here it covers the movement
signature scan against a real binary if one is present, and the input filter against a
stand-in game using SDL's dummy drivers. The extender's own suite — symbol resolution,
hooking, the patch ledger, and the plugin dispatch contract — lives in
[bg3lese](https://github.com/Nerocon/bg3lese) and runs with `make -C vendor/bg3lese test`.

## Prior art

- [**Norbyte/bg3se**](https://github.com/Norbyte/bg3se) — the Windows Script Extender.
  Windows-only by construction: it resolves functions from MSVC byte patterns that cannot
  match a clang-built Linux binary.
- [**Ch4nKyy/BG3WASD**](https://github.com/Ch4nKyy/BG3WASD) — the Windows WASD mod. It
  reaches the same movement actions from a different direction, by remapping keybinds and
  NOP-ing a controller-mode gate. Independent confirmation that the gate is the right lever.
- **ahungry/bg3-linux-ae** — the only other public native-Linux BG3 code mod. All four of
  its byte patterns score zero hits in build 4.1.1.7398727, which is the clearest argument
  for deriving addresses at runtime instead of hardcoding them.

## License

MIT — see [LICENSE](LICENSE).

This project contains no Larian code or assets. It is interoperability work against a
binary you own, and it reads that binary from your own installation at runtime.
Baldur's Gate 3 is © Larian Studios.
