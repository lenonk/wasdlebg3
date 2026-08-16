# bg3le — a native Linux script extender for Baldur's Gate 3

Norbyte's BG3 Script Extender is Windows-only; on Linux you get it by running the
Windows build under Proton. This project targets the **native Linux BG3 executable**
directly, using `LD_PRELOAD` instead of DLL injection. The first feature is WASD
character movement.

## Status

Early. What exists and is tested:

- **`src/symres.c`** — resolves BG3's *unexported* internal symbols at runtime.
  The game's own functions and its ECS type-index statics live in `.symtab`, which the
  loader never maps and `dlsym()` cannot see, so this re-reads the executable from disk
  and applies the process's load bias (via `dl_iterate_phdr`, which is ASLR- and
  PIE-correct). Resolving from the 152,416-symbol table takes about 13 ms.
- **`src/bg3le.c`** — the preloaded shim. Interposes `SDL_PollEvent`, tracks WASD state,
  optionally hides those keys from the game, and can synthesise analog stick motion.
- **`test/`** — a stand-in "game" that proves suppression and injection work.
  **No copy of BG3 is required to run the tests**, and no display is needed.

## Why `SDL_PollEvent` is the whole input story

The binary imports `SDL_PollEvent` and nothing else that can read input:
no `SDL_PeepEvents`, `SDL_WaitEvent`, `SDL_AddEventWatch`, `SDL_SetEventFilter`,
and notably no `SDL_GetKeyboardState`. Every keystroke, mouse motion and controller
axis the game will ever see passes through that one function, so interposing it gives
exact control rather than a best-effort race.

## Build and test

```sh
sudo apt install libsdl2-dev            # plus a C compiler
make test
```

`make test` runs both suites and asserts on the results.

## Analysis workspace

`analysis/` holds the reverse-engineering tooling. `build_index.py` parses the binary's
`.symtab` and its surviving `.rela.text` relocations into a SQLite database
(152,416 symbols and 2,986,688 cross-references); `bg3q.py` queries it and disassembles
with reloc-derived symbol names. See `analysis/BINARY-FACTS.md` for what the binary does
and does not give us.

The database is derived from Larian's copyrighted executable and is gitignored. Nothing
derived from the game binary is published here — the mod ships as source that reads the
user's own installed copy.

## Scope

Single-player use. Injecting input into multiplayer sessions is not a goal and is likely
to desync; see the design document for the reasoning.
