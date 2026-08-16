# bg3le — running it on your machine

This is a two-stage test. Stage 1 touches nothing and just proves the library loads and
finds what it expects. Stage 2 actually moves your character. Do them in order — if stage 1
looks wrong, stage 2 would only produce a confusing result.

Everything here assumes the **native Linux** build of BG3. If your install is the Windows
build under Proton, none of this applies; the checker in step 2 will tell you plainly.

---

## 1. Find your game binary

```sh
find ~ -name bg3 -type f 2>/dev/null
# typically: ~/.steam/steam/steamapps/common/Baldurs Gate 3/bin/bg3
```

If you only find `bg3.exe`, that's the Windows build.

## 2. Pre-flight check (no launching, completely safe)

```sh
./bg3le-check "/path/to/Baldurs Gate 3"
```

Give it either the game folder or the executable itself — handed a folder, it goes and finds
`bin/bg3`. **Quote the path**: "Baldurs Gate 3" has spaces in it, and an unquoted path gets
chopped up by the shell before the tool ever sees it.

You want to see `GOOD — this build is supported`, along with a build id, a version, and three
derived addresses. **Send me this output either way.** If it says NOT FOUND, your build differs
from the one I analysed and I need to re-derive the signature — that's a normal outcome, not a
failure, and it's exactly why this step exists.

For reference, the binary you uploaded reports:

```
build id      : c8da62b137caebd03b57c23b65ba2547745ce851
version       : 4.1.1.7398727
FOUND at        0x2c75c6a
state pointer @ 0x7d9d198
```

## 3. Put the library somewhere without spaces in the path

`LD_PRELOAD` and Steam launch options both handle spaces badly, and "Baldurs Gate 3" has two.

```sh
mkdir -p ~/bg3le && cp bg3le.so ~/bg3le/
```

## 4. Stage 1 — load it, write nothing

Easiest from a terminal, which avoids Steam's launch-option quoting entirely. Leave Steam
running in the background so the game finds `steam_api`.

```sh
cd "/path/to/Baldurs Gate 3/bin"
BG3LE_LOG=/tmp/bg3le.log BG3LE_MOVE=0 LD_PRELOAD=$HOME/bg3le/bg3le.so ./bg3
```

Load a save, walk around normally for a few seconds, quit, then:

```sh
cat /tmp/bg3le.log
```

**What a good log looks like:**

```
[bg3le  1234.567] loaded (suppress=1 move=0 inject=0)
[bg3le  1234.789] override signature @0x... : global 0x..., vec +0x1394, flag +0x139c
[bg3le  1234.790] host: 152416 symbols, load bias 0x...
[bg3le  1235.001] ECS type index @0x... = 42  (...ecl::Character...)
```

Things worth noticing:

- **Two `loaded` lines** would mean the game forks and does its work in a child process.
  That's expected per my analysis and harmless, but tell me if you see it.
- **A nonzero ECS type index** is a strong signal — it means we're reading live engine state
  correctly, not just guessing at addresses.
- **`signature not found`** with a real BG3 means step 2 and step 4 disagree; send me both.

If WASD stopped working in-game during this stage, that's expected: `suppress=1` hides those
keys. Add `BG3LE_SUPPRESS=0` if you want them passed through untouched.

## 5. Stage 2 — actually move

**Make a manual save first.** The risk is low — we write only a transient per-frame input
vector, never persistent state — but a save costs nothing.

```sh
cd "/path/to/Baldurs Gate 3/bin"
BG3LE_LOG=/tmp/bg3le.log LD_PRELOAD=$HOME/bg3le/bg3le.so ./bg3
```

Load a game, get a character selected and out of combat, and hold **W**.

There are three possible outcomes, and **all three are useful results** — this is the
experiment that static analysis could not settle:

| what happens | what it means |
|---|---|
| The character walks forward, camera-relative | It works. The mod is real; the rest is polish. |
| Nothing moves, but the log shows `movement control live` | `MoveController` isn't ticked in keyboard-and-mouse mode. We fall back to pinning controller mode. This is the outcome I consider most likely to need follow-up. |
| Log never shows `movement control live` | The state-block pointer stays null — our global is wrong, or that block is allocated somewhere we haven't looked. |

Send me `/tmp/bg3le.log` whichever way it goes.

## If it crashes

Not expected — we write 9 bytes to a location the game reads every frame anyway — but if it
does, `BG3LE_MOVE=0` disables all writing and gets you back to a pure observer. The game links
Crashpad, so a crash may be swallowed or uploaded; the log file is the reliable record.

## Environment variables

| variable | default | meaning |
|---|---|---|
| `BG3LE_LOG` | stderr | log file path |
| `BG3LE_MOVE` | `1` | write the movement vector (`0` = observe only) |
| `BG3LE_SUPPRESS` | `1` | hide WASD from the game's own hotkey handling |
| `BG3LE_INJECT` | `0` | fallback: synthesise gamepad stick events instead |

## Rebuilding

The prebuilt `.so` needs glibc 2.34 or newer and links only against libc. If it refuses to
load:

```sh
sudo apt install libsdl2-dev     # headers only, for the SDL_Event layout
make
```
