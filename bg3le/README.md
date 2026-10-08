# Linux Native WASD

WASD movement for the native Linux build of Baldur's Gate 3, as a
[bg3le](https://github.com/lenonk/bg3le) plugin.

It is [wasdlebg3](https://github.com/Nerocon/wasdlebg3) by Brian Calvert
(Nerocon), ported onto bg3le's plugin API: the same movement code, loaded by
bg3le from its plugins directory instead of by its own `LD_PRELOAD` host, so it
runs alongside bg3le and its other plugins. Thanks to Brian for the reverse
engineering it all rests on; how it works is in wasdlebg3's README and
`DESIGN.md`.

## Requirements

- [bg3le](https://www.nexusmods.com/baldursgate3/mods/25431) v0.3.5 or newer.
  Older releases can't call the plugin every frame, and it declines to load.
- The native Linux build of Baldur's Gate 3. Single-player only.

## Install

With the game closed, unzip and run `./install.py` (or `python3 install.py`).
It copies `linux_native_wasd.so` into `~/.local/share/bg3le/plugins`, where
bg3le loads it at the next launch. No launch options change.

If the game's launch options still preload wasdlebg3's own `bg3le.so` or
`bg3lese.so`, remove that, or movement is driven twice. The installer warns
when it finds one.

`./install.py --uninstall` removes the plugin and its settings.

## Controls

| input | what it does |
|---|---|
| **W A S D** | move, relative to the camera |
| **Left Shift** (hold) | walk instead of run |
| everything else | untouched |

WASD are hidden from the game while the plugin drives them, so they no longer
pan the camera. Typing in a text field leaves every key to the game, and losing
window focus releases movement.

## Settings

bg3le keeps them in `~/.local/share/bg3le/plugins/linux_native_wasd.settings.json`,
written on first launch. Edit it with the game closed, or change them from Lua
with `Ext.Plugins.Set("LinuxNativeWASD", id, value)`.

| setting | default | meaning |
|---|---|---|
| `walk_key` | `225` | SDL scancode of the walk modifier (225 is Left Shift, 229 Right Shift, 224 Left Ctrl); `0` turns walking off |
| `walk_speed` | `0.5` | walk speed as a fraction of run, 0.05 to 1 |
| `suppress` | `true` | hide WASD from the game's own bindings |
| `move` | `true` | drive movement (`false` observes only) |
| `gate` | `true` | open the game's controller-mode gate at load |
| `input_only` | `false` | filter input without moving, for diagnosing an unrecognised game build |
| `trace` | `false` | log every key and movement change |

Its lines in bg3le's log (`~/.local/share/bg3le/logs`) start with
`plugin LinuxNativeWASD:`. On a supported build they include
`opened 1 of 1 movement gate(s)` and, once a character is under control,
`movement control live`.

## Building

`bg3le/package.sh` builds the release zip against bg3le's Steam Runtime sysroot.
`make bg3le` builds `build/bg3le/linux_native_wasd.so` against the host.

## License

MIT, as wasdlebg3: see `LICENSE`.
