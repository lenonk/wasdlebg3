# bg3le — architecture for native Linux WASD movement

Target: Baldur's Gate 3 **4.1.1.7398727**, native Linux ELF, Build ID `c8da62b137caebd03b57c23b65ba2547745ce851`.

## Recommendation

Drive the game's **own analog-stick movement path** through a forced-input override that already
exists in the binary, from an `LD_PRELOAD` library that piggybacks on `SDL_PollEvent` for its
per-frame heartbeat.

**Version 1 requires no inline hooking, no code patching and no ECS surgery.** It is two memory
writes per frame into a global the game reads anyway. That is a dramatically smaller blast radius
than the Windows Script Extender's approach, and it exists because of a lucky find described below.

## The mechanism

`0x2c75c60` is the function that fetches the character movement vector each frame. Its first act is
to check a debug/forced-input override *before* polling anything:

```
0x02c75c6a  mov   rax, [rip + 0x5127527]   ; rax = *(void**)0x7D9D198   (global state block)
0x02c75c71  mov   rcx, [rip + 0x50E63C0]   ;       0x7D5C038  (playerId -> input index map)
0x02c75c78  cmp   byte [rax + 0x139C], 0   ; override flag
0x02c75c7f  je    0x2c75c8e                ; clear -> poll the four input actions
0x02c75c81  movsd xmm5, [rax + 0x1394]     ; set   -> take this vec2 and skip ALL polling
0x02c75c89  jmp   0x2c75d43                ;          straight to the shared normalise path
```

When the flag is clear it polls four analog input actions through `InputManager::GetActionValue`
at `0x2d1d880`:

| id | action |
|------|-------------------------|
| `0x9d` | `CharacterMoveForward` |
| `0x9e` | `CharacterMoveBackward` |
| `0x9f` | `CharacterMoveLeft` |
| `0xa0` | `CharacterMoveRight` |

Crucially, the override jumps to `0x2c75d43`, which is **upstream** of everything that matters:
magnitude, deadzone, normalise, unit-clamp, and then a camera-space rotation
(`call 0x2194aa0` at `0x2c75dd3`). So the vector we supply is **camera-relative, exactly like a
real analog stick** — "forward" means "away from the camera", which is precisely WASD semantics.
We never have to read camera yaw ourselves.

Downstream, the result lands in `eoc::controller::LocomotionComponent` and flows through the
engine's normal locomotion, animation, collision and network-sync path. The character does not
teleport or slide; it walks, because as far as the game is concerned nothing unusual happened.

So the whole of v1 is:

```c
struct { float x, y; } *forced = (void*)(*(uintptr_t*)(base + 0x7D9D198) + 0x1394);
uint8_t                *enable = (void*)(*(uintptr_t*)(base + 0x7D9D198) + 0x139C);
*forced = (vec2){ d - a, s - w };
*enable = (w || a || s || d);
```

...executed once per frame from inside our `SDL_PollEvent` interposer, which already runs on the
main thread at exactly the right point in the frame loop.

## What is verified versus assumed

Every claim below marked **verified** was checked directly against the binary in this workspace,
independently of the agent that first proposed it. The adversarial verification pass was lost to a
session limit, so the distinction matters.

**Verified here:**

- The forced-input override at `0x2c75c60`, including all three RIP-relative globals recomputed by
  hand: `0x2c75c71 + 0x5127527 = 0x7D9D198`, likewise `0x7D5C038` and `0x7D9D0A8`.
- `0x9d` is `CharacterMoveForward` — `mov dword [r14], 0x9d` at `0x414654a` is immediately followed
  by a `lea` of the string `CharacterMoveForward` with length `0x14` (= 20 = its strlen).
- The override path reaches deadzone/normalise/clamp and the camera rotation at `0x2c75dd3`.
- **Zero `endbr64` in all 90.5 MB of `.text`**, and no `.note.gnu.property` — no Intel CET, so
  inline hooking is unobstructed if we ever need it.
- `SDL_PollEvent` has **exactly one call site**, `0x02e9e7c7`. Control case: `SDL_WarpMouseInWindow`
  correctly shows three, so this is a real result and not a broken scanner.
- `0x2c72f89` (inside the claimed `MoveController::update` at `0x2c72d80`, at `+0x209`) is one of
  three direct callers of the input fetch.

**Reported by analysis but not independently re-verified** — treat as good leads, not facts:
`LocomotionComponent`'s 132-byte layout; `eocnet::CharacterSteeringMessage` id `0x24`; the camera
component layout and `GameCameraBehavior` direction vectors at `+0x70`/`+0x7c`; the input-mode
arbiter at `0x500c4f0` with `ActivateControllerMode` `0x500bec0` / `EnsureKBM` `0x500c270`; the
controller-mode suppression bytes at `[*(base+0x7D5C040)]+0x221/+0x222`; and the claim that the
game `fork()`s at `main+0x32f` and runs as a child under a `waitpid` supervisor.

That last one matters operationally: if true, our library is loaded into both processes and the
one doing the work is the child. Verify it before spending time debugging a "nothing happened" run.

## The critical unknown

**Is `MoveController::update` ticked while the game is in keyboard-and-mouse mode?**

If it is, v1 works as written and the UI never changes. If it is not — if `MoveController` only
runs once the game has entered controller mode — then we must additionally force that tick, and
the honest fallback is the controller-mode path, which carries a real cost: switching modes
triggers a wholesale Noesis UI reload (`Theme/DefaultTheme_c.xaml` versus `Theme/DefaultTheme.xaml`),
so a naive virtual gamepad plus a real mouse produces constant UI thrash between pad and KBM themes.
The reported escape hatch is pinning the mode via the two config bytes above.

This cannot be settled by static analysis. It needs the game installed and a 30-minute experiment,
which is the first thing to do once we have a real install.

Note this is precisely where our approach diverges favourably from the Windows prior art.
`Ch4nKyy/BG3WASD` gets WASD by remapping keybinds onto the `CharacterMove*` commands and then
NOP-ing a controller-mode gate. Our override sits *downstream* of the action polling, so if
`MoveController` ticks in KBM mode we bypass the gate entirely rather than defeating it.

## Patch resilience

Everything above except the symbol table is build-specific: `0x7D9D198` and `0x2c75c60` are
anonymous globals and functions with no symbols, so they will move on every patch.

Do not hardcode them. Instead **re-derive them at load time from a signature**, which is cheap and
robust here because the polling sequence is highly distinctive:

```
mov esi, 0xa0 / call rel32 / mov esi, 0x9f / call rel32 / mov esi, 0x9d / call rel32 / mov esi, 0x9e
```

Locate that, walk backwards to the `mov rax, [rip+disp32]` and the `cmp byte [rax+disp32], 0`, and
read the global address and both structure offsets out of the instruction encodings. That survives
recompilation as long as the mechanism itself survives, and it fails *loudly* — if the signature
does not match exactly once, refuse to load rather than corrupting a random global.

`analysis/callers.py` already does this class of scan and `src/symres.c` already resolves the
symbol table, so both halves exist.

The deeper fragility is that the entire approach rests on Larian shipping `.symtab` and
`--emit-relocs`. A future build that strips either one costs us the navigation advantage — though
not v1 itself, which only needs the signature scan.

## Risks

- **Multiplayer.** The client streams a derived target point and heading to the server. Injecting
  movement in co-op is untested and plausibly desyncs. Scope this to single-player and say so.
- **Saves.** v1 writes only a transient per-frame input vector, touching no persistent state, so
  save corruption risk is low. That stops being true the moment we start writing ECS components.
- **Crashpad is linked in.** It may swallow or upload our crashes. Disable it while developing.
- **Stuck movement.** If the flag is left set with a stale vector the character walks forever.
  Clear it whenever no key is held, and on focus loss.

## Staged plan

1. **The `.so` loads and logs.** Already done and tested — see below.
2. **Read-only proof of life.** With the game installed, run the signature scan and log the derived
   global, then log the live movement vector while the user moves with a real controller or mouse.
   No writes. This confirms every address in one shot.
3. **Move the character.** Enable the two writes. This is the milestone.
4. **Settle the KBM question**, and if needed add mode pinning.
5. Polish: run/walk modifier, focus handling, config file, jump/interact passthrough.
6. Only then consider ECS access, which is where the `ls::TypeId<…>::m_TypeIndex` statics
   (2,107 components, 934 systems, resolvable **by name**) become the foundation for a general
   extender rather than a movement mod.

## What already works

`make test` passes 17 assertions with no copy of BG3 required and no display:

- `src/symres.c` resolves unexported `.symtab` symbols at runtime, proven by calling through a
  resolved pointer under PIE + ASLR, and by matching four BG3 addresses against the analysis DB.
  `.symtab` sits past the end of every `PT_LOAD`, which is exactly why `dlsym` cannot substitute.
- `src/bg3le.c` interposes `SDL_PollEvent`, hides WASD from the game (6 key events → 2), passes
  unrelated keys through in order, and synthesises correctly normalised analog motion
  (diagonal = 23169 = 32767 × 0.707).

The next commit replaces the placeholder stick injection in `queue_axis_events()` with the
forced-input global write described above.

## Repository layout

```
bg3-extender/
  src/symres.{c,h}      runtime .symtab resolution (done)
  src/bg3le.c           the preloaded shim (input side done)
  src/sigscan.c         signature -> global/offset derivation   <- write next
  src/move.c            forced-input writer                      <- write next
  test/                 stand-in game + assertions (done)
  analysis/             bg3q.py, callers.py, build_index.py, BINARY-FACTS.md, dossier.json
```

## Prior art worth knowing

- **`Norbyte/bg3se`** — Windows only, a DWrite.dll proxy resolving 99 symbols from 69 byte patterns
  via Detours. Norbyte has stated those MSVC patterns cannot match a clang-built Linux binary;
  the Linux port issue has been open since the native build shipped.
- **`Ch4nKyy/BG3WASD`** — open source; keybind remap onto `CharacterMove*` plus one NOP'd gate.
  Confirms our understanding of the movement actions from a completely independent direction.
- **`ahungry/bg3-linux-ae`** — the only public native-Linux BG3 code mod, ~299 lines of
  `LD_PRELOAD` doing `mprotect` + `memcpy` byte patches located by scanning `/proc/self/maps`.
  All four of its historical byte patterns score **zero** hits in our build, which is the most
  concrete argument available for deriving addresses from symbols and signatures rather than
  hardcoded patterns.
